// SPDX-License-Identifier: MIT
#include <dtv/userland/control_client.h>

#include <dtv/userland/control_connection.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#if defined(DTV_USERLAND_CONTROL_STACK_PROBES)
#include <pthread.h>
#endif

#if defined(__SANITIZE_ADDRESS__)
#define DTV_USERLAND_CONTROL_ADDRESS_SANITIZER 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define DTV_USERLAND_CONTROL_ADDRESS_SANITIZER 1
#endif
#endif

namespace {

namespace w = dtv::userland::wire;
using dtv::userland::ByteView;
using dtv::userland::Error;
using dtv::userland::MutableByteView;
using dtv::userland::Result;

int g_checks = 0;
int g_failures = 0;

#define CHECK(condition)                                                                          \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(condition)) {                                                                        \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);             \
        }                                                                                          \
    } while (0)

constexpr std::size_t kFrameCapacity = w::kHeaderSize + w::kMaxControlPayload;

template <std::size_t N>
void set_text(w::WString<N>& out, const char* text)
{
    out.length = 0U;
    while (text[out.length] != '\0') {
        out.bytes[out.length] = text[out.length];
        ++out.length;
    }
}

w::ServerHello make_server(std::uint64_t features = 0U)
{
    w::ServerHello server;
    for (std::size_t i = 0U; i < server.epoch.size(); ++i) {
        server.epoch[i] = static_cast<std::uint8_t>(0xa0U + i);
    }
    set_text(server.producer.product, "test-product");
    set_text(server.producer.version, "1.2.3");
    server.features = features;
    return server;
}

void sparse_list(w::ListResponse& list, const w::Uuid& epoch)
{
    list = w::ListResponse{};
    list.epoch = epoch;
    set_text(list.producer.product, "test-product");
    set_text(list.producer.version, "1.2.3");
    list.topology.state = w::TopologyState::complete;
    list.topology.expected_function_count.present = true;
    list.topology.expected_function_count.value = 1U;
    list.enclosure.present = true;
    list.enclosure.value.identity.serial_state = w::IdentitySerialState::absent;
    list.enclosure.value.identity.uniqueness = w::Uniqueness::not_applicable;
    list.functions.count = 1U;
    list.functions.items[0].id = 0U;
    list.functions.items[0].vid = 0x1234U;
    list.functions.items[0].pid = 0x5678U;
    list.functions.items[0].mode = w::Mode::runtime;
    list.functions.items[0].serial_state = w::SerialState::absent;
    list.functions.items[0].present = true;
    list.functions.items[0].identification_reason = w::IdentificationReason::model_unconfirmed;
    list.receivers.present = true;
    list.receivers.value.count = 1U;
    list.receivers.value.items[0].id = 513U;
    list.receivers.value.items[0].systems.mask = 0x01U;
    list.receivers.value.items[0].binding.present = true;
    list.receivers.value.items[0].binding.value = w::Binding{0U, 0U};
    list.readers.present = true;
    list.readers.value.count = 0U;
}

enum class Fault : std::uint8_t {
    none,
    bad_hello_version,
    unrequested_hello_feature,
    wrong_list_request_id,
    wrong_status_opcode,
    bad_magic,
    truncated_status,
    wrong_list_epoch,
    wrong_status_epoch,
    noncanonical_status,
    remote_list_error,
    remote_tune_error,
    timeout_list,
    timeout_start,
    timeout_stop,
    timeout_release,
    release_remote_error,
    release_wire_error,
    release_without_error,
    bad_release_epoch,
    lnb_remote_error,
    lnb_eof,
    timeout_lnb,
    bad_lnb_epoch,
    bad_lnb_enabled,
    truncated_lnb,
    card_status_remote_error,
    card_connect_remote_error,
    card_disconnect_remote_error,
    card_eof,
    card_disconnect_eof,
    timeout_card,
    bad_card_status_epoch,
    bad_card_connect_epoch,
    bad_card_connect_generation,
    bad_disconnect_key,
    bad_disconnect_handle,
    card_not_consumed,
    card_disconnect_unquiesced_error,
    card_disconnect_unquiesced_without_error,
    card_reset_remote_error,
    card_reset_bad_handle,
    card_reset_bad_generation,
    card_reset_eof,
    truncated_card_reset,
    card_reset_unusable,
    timeout_card_reset,
    card_transmit_remote_error,
    card_transmit_bad_key,
    card_transmit_eof,
    truncated_card_transmit,
    timeout_card_transmit,
    card_transmit_maximum,
    truncated_card_connect,
};

class FakeTransport final : public w::ClientTransport, public w::TimedClientTransport {
public:
    explicit FakeTransport(Fault fault = Fault::none, bool accept_pid_requests = false,
                           std::uint64_t server_features = 0U,
                           bool card_reader_available = false)
        : server_(make_server(server_features)), connection_(server_), fault_(fault),
          accept_pid_requests_(accept_pid_requests)
    {
        sparse_list(list_, server_.epoch);
        if (card_reader_available) {
            list_.readers.value.count = 1U;
            list_.readers.value.items[0U].id = 23U;
        }
        context_.epoch = server_.epoch;
        context_.state = w::ContextState::ready;
        snapshot_ = w::Snapshot{3U, 5000U};
    }

    Result<void> write_all(ByteView bytes) noexcept override
    {
        if (closed_ || reply_offset_ != reply_size_ || request_count_ >= request_ids_.size()) {
            return Result<void>::failure(Error::PROTOCOL_ERROR);
        }
        const auto frame = w::decode_frame(bytes);
        if (!frame || frame.value().header.kind != w::Kind::request) {
            return Result<void>::failure(Error::PROTOCOL_ERROR);
        }
        const std::uint32_t request_id = frame.value().header.request_id;
        const std::uint16_t opcode = frame.value().header.opcode;
        request_ids_[request_count_] = request_id;
        request_opcodes_[request_count_] = opcode;
        ++request_count_;

        w::ControlRequest request{};
        const w::Inbound inbound = connection_.accept(
            frame.value(), request, MutableByteView{reply_.data(), reply_.size()});
        if (inbound.action == w::InboundAction::close) {
            return Result<void>::failure(Error::PROTOCOL_ERROR);
        }
        if (request.kind == w::ControlRequestKind::tune && request.tune.pids.present) {
            tune_pid_count_ = request.tune.pids.value.count;
            tune_pid_first_ = request.tune.pids.value.items[0U];
            tune_pid_last_ = request.tune.pids.value.items[tune_pid_count_ - 1U];
        }
        if (request.kind == w::ControlRequestKind::lnb_power)
            last_lnb_enabled_ = request.lnb_power.enabled;
        // PID fixtures isolate client request sizing from feature negotiation.
        if (inbound.action == w::InboundAction::respond &&
            !(accept_pid_requests_ && request.kind == w::ControlRequestKind::tune &&
              request.tune.pids.present)) {
            reply_size_ = inbound.reply_size;
        } else if (!queue_dispatch(request)) {
            return Result<void>::failure(Error::PROTOCOL_ERROR);
        }
        connection_.complete();
        reply_offset_ = 0U;
        apply_fault(opcode, request_id);
        if (reply_size_ != 0U && request_count_ <= recorded_replies_.size()) {
            const std::size_t index = request_count_ - 1U;
            std::memcpy(recorded_replies_[index].data(), reply_.data(), reply_size_);
            recorded_sizes_[index] = reply_size_;
        }
        return Result<void>::success();
    }

    Result<std::size_t> read_some(MutableByteView bytes) noexcept override
    {
        if (bytes.data == nullptr && bytes.size != 0U) {
            return Result<std::size_t>::failure(Error::INVALID_ARGUMENT);
        }
        if (reply_offset_ == reply_size_) {
            return Result<std::size_t>::success(0U);
        }
        const std::size_t remaining = reply_size_ - reply_offset_;
        const std::size_t count = remaining < 13U ? remaining : 13U;
        if (bytes.size < count) {
            return Result<std::size_t>::failure(Error::BUFFER_TOO_SMALL);
        }
        std::memcpy(bytes.data, reply_.data() + reply_offset_, count);
        reply_offset_ += count;
        return Result<std::size_t>::success(count);
    }

    Result<void> write_all(ByteView bytes, dtv::userland::Timeout timeout) noexcept override
    {
        if (write_timeout_count_ < write_timeouts_.size())
            write_timeouts_[write_timeout_count_++] = timeout.milliseconds;
        return write_all(bytes);
    }

    Result<std::size_t> read_some(MutableByteView bytes,
                                  dtv::userland::Timeout timeout) noexcept override
    {
        if (read_timeout_count_ < read_timeouts_.size())
            read_timeouts_[read_timeout_count_++] = timeout.milliseconds;
        if ((fault_ == Fault::timeout_list && request_count_ == 2U) ||
            (fault_ == Fault::timeout_start && request_count_ == 6U) ||
            (fault_ == Fault::timeout_stop && request_count_ == 7U) ||
            (fault_ == Fault::timeout_release && request_count_ == 8U) ||
            (fault_ == Fault::timeout_lnb && request_count_ == 4U) ||
            (fault_ == Fault::timeout_card && request_count_ == 5U) ||
            (fault_ == Fault::timeout_card_reset && request_count_ > 5U &&
             request_opcodes_[request_count_ - 1U] == w::kOpcodeCardReset) ||
            (fault_ == Fault::timeout_card_transmit && request_count_ > 5U &&
             request_opcodes_[request_count_ - 1U] == w::kOpcodeCardTransmit)) {
            now_ms_ += timeout.milliseconds;
            return Result<std::size_t>::failure(Error::TIMEOUT);
        }
        const auto read = read_some(bytes);
        if (read && advance_per_read_ms_ != 0U) now_ms_ += advance_per_read_ms_;
        return read;
    }

    std::uint64_t monotonic_ms() noexcept override { return now_ms_; }

    void close() noexcept override { closed_ = true; }

    bool closed() const noexcept { return closed_; }
    std::size_t request_count() const noexcept { return request_count_; }
    std::size_t tune_pid_count() const noexcept { return tune_pid_count_; }
    std::uint16_t tune_pid_first() const noexcept { return tune_pid_first_; }
    std::uint16_t tune_pid_last() const noexcept { return tune_pid_last_; }
    std::uint32_t request_id(std::size_t index) const noexcept { return request_ids_[index]; }
    std::uint16_t request_opcode(std::size_t index) const noexcept { return request_opcodes_[index]; }
    std::uint32_t write_timeout(std::size_t index) const noexcept { return write_timeouts_[index]; }
    std::uint32_t read_timeout(std::size_t index) const noexcept { return read_timeouts_[index]; }
    std::size_t write_timeout_count() const noexcept { return write_timeout_count_; }
    std::size_t read_timeout_count() const noexcept { return read_timeout_count_; }
    bool last_lnb_enabled() const noexcept { return last_lnb_enabled_; }
    void advance_per_read(std::uint32_t milliseconds) noexcept
    {
        advance_per_read_ms_ = milliseconds;
    }
    const std::uint8_t* recorded_reply(std::size_t index) const noexcept
    {
        return recorded_replies_[index].data();
    }
    std::size_t recorded_size(std::size_t index) const noexcept { return recorded_sizes_[index]; }

private:
    bool queue_dispatch(const w::ControlRequest& request) noexcept
    {
        if (request.kind == w::ControlRequestKind::list) {
            if (fault_ == Fault::remote_list_error) {
                const auto encoded = w::encode_error_frame(
                    request.opcode, request.request_id, server_.epoch,
                    static_cast<std::uint32_t>(Error::UNSUPPORTED), "list",
                    w::Scope{w::ScopeKind::enclosure, 0U},
                    MutableByteView{reply_.data(), reply_.size()});
                if (!encoded) return false;
                reply_size_ = encoded.value();
                return true;
            }
            response_list_ = list_;
            w::ListResponse& response = response_list_;
            if (fault_ == Fault::wrong_list_epoch) response.epoch[0] ^= 0x01U;
            const auto encoded = w::encode_record_frame(
                w::Kind::response, request.opcode, request.request_id, response,
                w::encode_list_response, MutableByteView{reply_.data(), reply_.size()});
            if (!encoded) return false;
            reply_size_ = encoded.value();
            return true;
        }
        if (request.kind == w::ControlRequestKind::status) {
            response_status_ = w::StatusResponse{};
            w::StatusResponse& response = response_status_;
            if (w::build_status_shell(list_, context_, snapshot_,
                                      w::Scope{w::ScopeKind::enclosure, 0U}, response) != Error::OK) {
                return false;
            }
            if (response.readers.present && response.readers.value.count != 0U)
                response.readers.value.items[0U].card.reason =
                    static_cast<std::uint8_t>(w::UnknownReason::not_sampled);
            if (fault_ == Fault::wrong_status_epoch) response.context.epoch[0] ^= 0x01U;
            if (fault_ == Fault::noncanonical_status && response.receivers.present) {
                response.receivers.value.items[0].descriptor.systems.mask = 0x03U;
            }
            Result<std::size_t> encoded =
                fault_ == Fault::wrong_status_epoch || fault_ == Fault::noncanonical_status
                    ? w::encode_record_frame(w::Kind::response, request.opcode,
                                             request.request_id, response,
                                             w::encode_status_response,
                                             MutableByteView{reply_.data(), reply_.size()})
                    : w::encode_status_frame(request.request_id, response, list_,
                                             MutableByteView{reply_.data(), reply_.size()});
            if (!encoded) return false;
            reply_size_ = encoded.value();
            return true;
        }
        if (request.kind == w::ControlRequestKind::lnb_power) {
            if (fault_ == Fault::lnb_remote_error) {
                const auto encoded = w::encode_error_frame(
                    request.opcode, request.request_id, server_.epoch,
                    static_cast<std::uint32_t>(Error::UNSUPPORTED), "lnb_power",
                    w::Scope{w::ScopeKind::enclosure, 0U},
                    MutableByteView{reply_.data(), reply_.size()});
                if (!encoded) return false;
                reply_size_ = encoded.value();
                return true;
            }
            if (fault_ == Fault::lnb_eof) {
                reply_size_ = 0U;
                return true;
            }
            w::LnbPowerResponse response{};
            response.epoch = server_.epoch;
            response.enabled = request.lnb_power.enabled;
            if (fault_ == Fault::bad_lnb_epoch) response.epoch[0] ^= 0x01U;
            if (fault_ == Fault::bad_lnb_enabled) response.enabled = !response.enabled;
            const auto encoded = w::encode_record_frame(
                w::Kind::response, request.opcode, request.request_id, response,
                w::encode_lnb_power_response, MutableByteView{reply_.data(), reply_.size()});
            if (!encoded) return false;
            reply_size_ = encoded.value();
            return true;
        }
        if (request.kind == w::ControlRequestKind::card_status) {
            if (fault_ == Fault::card_status_remote_error) {
                const auto encoded = w::encode_error_frame(
                    request.opcode, request.request_id, server_.epoch,
                    static_cast<std::uint32_t>(Error::UNSUPPORTED), "card_status",
                    w::Scope{w::ScopeKind::reader, request.card_status.key.reader_id},
                    MutableByteView{reply_.data(), reply_.size()});
                if (!encoded) return false;
                reply_size_ = encoded.value();
                return true;
            }
            w::CardStatusResponse response{};
            response.key = request.card_status.key;
            response.current_generation = 9U;
            response.present = true;
            response.initialized = true;
            response.protocol.present = true;
            response.protocol.value = w::CardProtocol::t1;
            response.atr.present = true;
            response.atr.value.length = 2U;
            response.atr.value.bytes[0U] = 0x3bU;
            response.atr.value.bytes[1U] = 0x00U;
            if (fault_ == Fault::bad_card_status_epoch) response.key.epoch[0U] ^= 0x01U;
            const auto encoded = w::encode_record_frame(
                w::Kind::response, request.opcode, request.request_id, response,
                w::encode_card_status_response, MutableByteView{reply_.data(), reply_.size()});
            if (!encoded) return false;
            reply_size_ = encoded.value();
            return true;
        }
        if (request.kind == w::ControlRequestKind::card_connect) {
            if (fault_ == Fault::card_connect_remote_error) {
                const auto encoded = w::encode_error_frame(
                    request.opcode, request.request_id, server_.epoch,
                    static_cast<std::uint32_t>(Error::NO_CARD), "card_connect",
                    w::Scope{w::ScopeKind::reader, request.card_connect.key.reader_id},
                    MutableByteView{reply_.data(), reply_.size()});
                if (!encoded) return false;
                reply_size_ = encoded.value();
                return true;
            }
            w::CardSessionResult response{};
            response.epoch = server_.epoch;
            response.reader_id = request.card_connect.key.reader_id;
            response.card_handle = 0x1234U;
            response.bound_generation = 9U;
            response.current_generation = 9U;
            response.present = true;
            response.initialized = true;
            response.usable = true;
            response.protocol.present = true;
            response.protocol.value = w::CardProtocol::t1;
            response.atr.present = true;
            response.atr.value.length = 2U;
            response.atr.value.bytes[0U] = 0x3bU;
            response.atr.value.bytes[1U] = 0x00U;
            if (fault_ == Fault::bad_card_connect_epoch) response.epoch[0U] ^= 0x01U;
            if (fault_ == Fault::bad_card_connect_generation) response.current_generation++;
            const auto encoded = w::encode_record_frame(
                w::Kind::response, request.opcode, request.request_id, response,
                w::encode_card_session_result, MutableByteView{reply_.data(), reply_.size()});
            if (!encoded) return false;
            reply_size_ = encoded.value();
            return true;
        }
        if (request.kind == w::ControlRequestKind::card_disconnect) {
            if (fault_ == Fault::card_disconnect_remote_error) {
                const auto encoded = w::encode_error_frame(
                    request.opcode, request.request_id, server_.epoch,
                    w::kWireErrorStaleHandle, "card_disconnect",
                    w::Scope{w::ScopeKind::reader, request.card_disconnect.key.reader_id},
                    MutableByteView{reply_.data(), reply_.size()});
                if (!encoded) return false;
                reply_size_ = encoded.value();
                return true;
            }
            w::CardDisconnectResponse response{};
            response.key.epoch = request.card_disconnect.key.epoch;
            response.key.reader_id = request.card_disconnect.key.reader_id;
            response.card_handle = request.card_disconnect.key.card_handle;
            response.consumed = fault_ != Fault::card_not_consumed;
            response.quiesced = fault_ != Fault::card_disconnect_unquiesced_error &&
                                fault_ != Fault::card_disconnect_unquiesced_without_error;
            if (fault_ == Fault::card_disconnect_unquiesced_error) {
                response.error.present = true;
                response.error.value.code = static_cast<std::uint32_t>(Error::USB_IO);
            }
            if (fault_ == Fault::bad_disconnect_key) response.key.reader_id++;
            if (fault_ == Fault::bad_disconnect_handle) response.card_handle++;
            const auto encoded = w::encode_record_frame(
                w::Kind::response, request.opcode, request.request_id, response,
                w::encode_card_disconnect_response,
                MutableByteView{reply_.data(), reply_.size()});
            if (!encoded) return false;
            reply_size_ = encoded.value();
            return true;
        }
        if (request.kind == w::ControlRequestKind::card_reset) {
            if (fault_ == Fault::card_reset_remote_error) {
                const auto encoded = w::encode_error_frame(
                    request.opcode, request.request_id, server_.epoch,
                    static_cast<std::uint32_t>(Error::CARD_REMOVED), "card_reset",
                    w::Scope{w::ScopeKind::reader, request.card_reset.key.reader_id},
                    MutableByteView{reply_.data(), reply_.size()});
                if (!encoded) return false;
                reply_size_ = encoded.value();
                return true;
            }
            w::CardSessionResult response{};
            response.epoch = request.card_reset.key.epoch;
            response.reader_id = request.card_reset.key.reader_id;
            response.card_handle = request.card_reset.key.card_handle;
            response.bound_generation = 10U;
            response.current_generation = 10U;
            response.usable = fault_ != Fault::card_reset_unusable;
            if (response.usable) {
                response.present = true;
                response.initialized = true;
                response.protocol.present = true;
                response.protocol.value = w::CardProtocol::t1;
                response.atr.present = true;
                response.atr.value.length = 3U;
                response.atr.value.bytes[0U] = 0x3bU;
                response.atr.value.bytes[1U] = 0x01U;
                response.atr.value.bytes[2U] = 0x02U;
            }
            if (fault_ == Fault::card_reset_bad_handle) response.card_handle++;
            if (fault_ == Fault::card_reset_bad_generation)
                response.current_generation++;
            const auto encoded = w::encode_record_frame(
                w::Kind::response, request.opcode, request.request_id, response,
                w::encode_card_session_result,
                MutableByteView{reply_.data(), reply_.size()});
            if (!encoded) return false;
            reply_size_ = encoded.value();
            return true;
        }
        if (request.kind == w::ControlRequestKind::card_transmit) {
            if (fault_ == Fault::card_transmit_remote_error) {
                const auto encoded = w::encode_error_frame(
                    request.opcode, request.request_id, server_.epoch,
                    static_cast<std::uint32_t>(Error::CARD_REMOVED), "card_transmit",
                    w::Scope{w::ScopeKind::reader, request.card_transmit.key.reader_id},
                    MutableByteView{reply_.data(), reply_.size()});
                if (!encoded) return false;
                reply_size_ = encoded.value();
                return true;
            }
            w::CardTransmitResponse response{};
            response.key = request.card_transmit.key;
            if (fault_ == Fault::card_transmit_bad_key) response.key.bound_generation++;
            response.response.length = fault_ == Fault::card_transmit_maximum
                                           ? static_cast<std::uint32_t>(w::kMaxApduBytes)
                                           : 2U;
            response.response.bytes[0U] = 0x90U;
            response.response.bytes[1U] = 0x00U;
            const auto encoded = w::encode_record_frame(
                w::Kind::response, request.opcode, request.request_id, response,
                w::encode_card_transmit_response,
                MutableByteView{reply_.data(), reply_.size()});
            if (!encoded) return false;
            reply_size_ = encoded.value();
            return true;
        }
        if (request.kind == w::ControlRequestKind::acquire) {
            w::AcquireResponse response{};
            response.epoch = server_.epoch;
            response.lease_id = 77U;
            const auto encoded = w::encode_record_frame(
                w::Kind::response, request.opcode, request.request_id, response,
                w::encode_acquire_response, MutableByteView{reply_.data(), reply_.size()});
            if (!encoded) return false;
            reply_size_ = encoded.value();
            return true;
        }
        if (request.kind == w::ControlRequestKind::tune) {
            if (fault_ == Fault::remote_tune_error) {
                const auto encoded = w::encode_error_frame(
                    request.opcode, request.request_id, server_.epoch, 999U, "tune",
                    w::Scope{w::ScopeKind::receiver, request.tune.lease_id},
                    MutableByteView{reply_.data(), reply_.size()});
                if (!encoded) return false;
                reply_size_ = encoded.value();
                return true;
            }
            w::TuneResult response{};
            response.epoch = server_.epoch;
            response.tune_revision = 3U;
            response.requested_hz = 473142857U;
            const auto encoded = w::encode_record_frame(
                w::Kind::response, request.opcode, request.request_id, response,
                w::encode_tune_result, MutableByteView{reply_.data(), reply_.size()});
            if (!encoded) return false;
            reply_size_ = encoded.value();
            return true;
        }
        if (request.kind == w::ControlRequestKind::start_stream) {
            w::StartResponse response{};
            response.epoch = server_.epoch;
            response.stream_id = 99U;
            response.attach_token[0] = 0x33U;
            response.expires_after_ms = 1000U;
            const auto encoded = w::encode_record_frame(
                w::Kind::response, request.opcode, request.request_id, response,
                w::encode_start_response, MutableByteView{reply_.data(), reply_.size()});
            if (!encoded) return false;
            reply_size_ = encoded.value();
            return true;
        }
        if (request.kind == w::ControlRequestKind::stop_stream) {
            w::StopResponse response{};
            response.epoch = server_.epoch;
            response.final.id = request.stop_stream.stream_id;
            response.final.tune_revision = 3U;
            response.quiesced = true;
            const auto encoded = w::encode_record_frame(
                w::Kind::response, request.opcode, request.request_id, response,
                w::encode_stop_response, MutableByteView{reply_.data(), reply_.size()});
            if (!encoded) return false;
            reply_size_ = encoded.value();
            return true;
        }
        if (request.kind == w::ControlRequestKind::release) {
            if (fault_ == Fault::release_wire_error) {
                const auto encoded = w::encode_error_frame(
                    request.opcode, request.request_id, server_.epoch, 999U, "release",
                    w::Scope{w::ScopeKind::receiver, request.release.lease_id},
                    MutableByteView{reply_.data(), reply_.size()});
                if (!encoded) return false;
                reply_size_ = encoded.value();
                return true;
            }
            w::ReleaseResponse response{};
            response.epoch = server_.epoch;
            if (fault_ == Fault::bad_release_epoch) response.epoch[0] ^= 0x01U;
            response.lease_id = request.release.lease_id;
            response.released = fault_ != Fault::release_remote_error &&
                                fault_ != Fault::release_without_error;
            response.quiesced = response.released;
            if (fault_ == Fault::release_remote_error) {
                response.error.present = true;
                response.error.value.code = 999U;
            }
            const auto encoded = w::encode_record_frame(
                w::Kind::response, request.opcode, request.request_id, response,
                w::encode_release_response, MutableByteView{reply_.data(), reply_.size()});
            if (!encoded) return false;
            reply_size_ = encoded.value();
            return true;
        }
        return false;
    }

    void apply_fault(std::uint16_t opcode, std::uint32_t request_id) noexcept
    {
        if (fault_ == Fault::bad_hello_version && opcode == w::kOpcodeHello) {
            reply_[w::kHeaderSize] = 2U;
        } else if (fault_ == Fault::unrequested_hello_feature && opcode == w::kOpcodeHello) {
            reply_[w::kHeaderSize + 4U] = 1U;
        } else if (fault_ == Fault::wrong_list_request_id && opcode == w::kOpcodeList) {
            rewrite_header(static_cast<std::uint32_t>(request_id + 1U), opcode);
        } else if (fault_ == Fault::wrong_status_opcode && opcode == w::kOpcodeStatus) {
            rewrite_header(request_id, w::kOpcodeList);
        } else if (fault_ == Fault::bad_magic && opcode == w::kOpcodeList) {
            reply_[0] ^= 0xffU;
        } else if (fault_ == Fault::truncated_status && opcode == w::kOpcodeStatus &&
                   reply_size_ != 0U) {
            --reply_size_;
        } else if (fault_ == Fault::truncated_lnb && opcode == w::kOpcodeLnbPower &&
                   reply_size_ != 0U) {
            --reply_size_;
        } else if (fault_ == Fault::card_eof && opcode == w::kOpcodeCardConnect) {
            reply_size_ = 0U;
        } else if (fault_ == Fault::card_disconnect_eof &&
                   opcode == w::kOpcodeCardDisconnect) {
            reply_size_ = 0U;
        } else if (fault_ == Fault::card_reset_eof && opcode == w::kOpcodeCardReset) {
            reply_size_ = 0U;
        } else if (fault_ == Fault::card_transmit_eof &&
                   opcode == w::kOpcodeCardTransmit) {
            reply_size_ = 0U;
        } else if ((fault_ == Fault::truncated_card_reset &&
                    opcode == w::kOpcodeCardReset) ||
                   (fault_ == Fault::truncated_card_transmit &&
                    opcode == w::kOpcodeCardTransmit)) {
            if (reply_size_ != 0U) --reply_size_;
        } else if (fault_ == Fault::truncated_card_connect &&
                   opcode == w::kOpcodeCardConnect && reply_size_ != 0U) {
            --reply_size_;
        }
    }

    void rewrite_header(std::uint32_t request_id, std::uint16_t opcode) noexcept
    {
        const auto decoded = w::decode_frame(ByteView{reply_.data(), reply_size_});
        if (!decoded) return;
        w::Header header = decoded.value().header;
        header.request_id = request_id;
        header.opcode = opcode;
        const auto encoded = w::encode_frame(
            header, decoded.value().payload,
            MutableByteView{scratch_.data(), scratch_.size()});
        if (encoded) {
            std::memcpy(reply_.data(), scratch_.data(), encoded.value());
            reply_size_ = encoded.value();
        }
    }

    w::ServerHello server_;
    w::ControlConnection connection_;
    w::ListResponse list_{};
    w::ListResponse response_list_{};
    w::StatusResponse response_status_{};
    w::Context context_{};
    w::Snapshot snapshot_{};
    Fault fault_;
    std::array<std::uint8_t, kFrameCapacity> reply_{};
    std::array<std::uint8_t, kFrameCapacity> scratch_{};
    std::size_t reply_size_ = 0U;
    std::size_t reply_offset_ = 0U;
    std::array<std::uint32_t, 8U> request_ids_{};
    std::array<std::uint16_t, 8U> request_opcodes_{};
    std::array<std::uint32_t, 64U> write_timeouts_{};
    std::array<std::uint32_t, 64U> read_timeouts_{};
    std::array<std::array<std::uint8_t, kFrameCapacity>, 3U> recorded_replies_{};
    std::array<std::size_t, 3U> recorded_sizes_{};
    std::size_t request_count_ = 0U;
    std::size_t tune_pid_count_ = 0U;
    std::uint16_t tune_pid_first_ = 0U;
    std::uint16_t tune_pid_last_ = 0U;
    std::size_t write_timeout_count_ = 0U;
    std::size_t read_timeout_count_ = 0U;
    std::uint32_t advance_per_read_ms_ = 0U;
    std::uint64_t now_ms_ = 0U;
    bool closed_ = false;
    bool accept_pid_requests_ = false;
    bool last_lnb_enabled_ = false;
};

void test_successful_read_only_exchange()
{
    FakeTransport transport;
    const w::StatusFetchResult result = w::fetch_status(transport);
    CHECK(result);
    CHECK(!transport.closed());
    CHECK(transport.request_count() == 3U);
    CHECK(transport.request_id(0U) == 1U && transport.request_id(1U) == 2U &&
          transport.request_id(2U) == 3U);
    CHECK(transport.request_opcode(0U) == w::kOpcodeHello &&
          transport.request_opcode(1U) == w::kOpcodeList &&
          transport.request_opcode(2U) == w::kOpcodeStatus);
    CHECK(result.snapshot.list.epoch == result.snapshot.status.context.epoch);
    CHECK(result.snapshot.status.scope.kind == w::ScopeKind::enclosure &&
          result.snapshot.status.scope.id == 0U);
    CHECK(result.snapshot.list.receivers.value.count == 1U &&
          result.snapshot.status.receivers.value.count == 1U);
}

void test_invalid_responses_close_without_publishing_snapshot()
{
    const Fault faults[] = {Fault::bad_hello_version, Fault::unrequested_hello_feature,
                            Fault::wrong_list_request_id, Fault::wrong_status_opcode, Fault::bad_magic,
                            Fault::truncated_status, Fault::wrong_list_epoch,
                            Fault::wrong_status_epoch, Fault::noncanonical_status};
    for (const Fault fault : faults) {
        FakeTransport transport(fault);
        const w::StatusFetchResult result = w::fetch_status(transport);
        CHECK(!result);
        CHECK(transport.closed());
        CHECK(result.snapshot.list.epoch == w::Uuid{});
    }
}

void test_remote_error_code_is_preserved()
{
    FakeTransport transport(Fault::remote_list_error);
    const w::StatusFetchResult result = w::fetch_status(transport);
    CHECK(!result && result.error == Error::UNSUPPORTED);
    CHECK(result.remote_error_code == static_cast<std::uint32_t>(Error::UNSUPPORTED));
    CHECK(transport.closed());
    CHECK(result.snapshot.list.epoch == w::Uuid{});
}

Error accept_status_epoch(const w::StatusSnapshot& snapshot, void* context) noexcept
{
    const auto* expected = static_cast<const w::Uuid*>(context);
    return expected != nullptr && snapshot.status.context.epoch == *expected
               ? Error::OK
               : Error::NOT_FOUND;
}

Error reject_status(const w::StatusSnapshot&, void*) noexcept
{
    return Error::NOT_FOUND;
}

void test_timed_status_uses_one_deadline_and_preserves_old_transport()
{
    FakeTransport transport;
    transport.advance_per_read(1U);
    const w::StatusFetchResult result = w::fetch_status(transport, dtv::userland::Timeout{1000U});
    CHECK(result);
    CHECK(!transport.closed());
    CHECK(transport.request_count() == 3U);
    CHECK(transport.write_timeout(0U) == 1000U);
    CHECK(transport.read_timeout(0U) == 1000U);
    CHECK(transport.read_timeout(1U) < 1000U);
    CHECK(transport.write_timeout(1U) < 1000U);

    FakeTransport zero_timeout;
    const w::StatusFetchResult invalid =
        w::fetch_status(zero_timeout, dtv::userland::Timeout{0U});
    CHECK(!invalid && invalid.error == Error::INVALID_ARGUMENT);
    CHECK(zero_timeout.closed() && zero_timeout.request_count() == 0U);
}

void test_timed_status_required_features()
{
    const std::uint64_t supported_features =
        w::kFeatureCardService | w::kFeatureStateEvent;
    FakeTransport supported(Fault::none, false, supported_features);
    const auto result = w::fetch_status(
        supported, dtv::userland::Timeout{1000U}, w::kFeatureCardService);
    CHECK(result);
    CHECK(!supported.closed() && supported.request_count() == 3U);
    CHECK(supported.request_id(0U) == 1U && supported.request_opcode(0U) == w::kOpcodeHello);
    CHECK(supported.request_id(1U) == 2U && supported.request_opcode(1U) == w::kOpcodeList);
    CHECK(supported.request_id(2U) == 3U && supported.request_opcode(2U) == w::kOpcodeStatus);

    FakeTransport missing;
    const auto missing_result = w::fetch_status(
        missing, dtv::userland::Timeout{1000U}, w::kFeatureCardService);
    CHECK(!missing_result && missing_result.error == Error::UNSUPPORTED);
    CHECK(missing_result.remote_error_code == static_cast<std::uint32_t>(Error::UNSUPPORTED));
    CHECK(missing.closed() && missing.request_count() == 1U);
    CHECK(missing.request_opcode(0U) == w::kOpcodeHello);

    FakeTransport unknown;
    const std::uint64_t unknown_feature = std::uint64_t{1U} << 63U;
    const auto unknown_result = w::fetch_status(
        unknown, dtv::userland::Timeout{1000U}, unknown_feature);
    CHECK(!unknown_result && unknown_result.error == Error::UNSUPPORTED);
    CHECK(unknown_result.remote_error_code == static_cast<std::uint32_t>(Error::UNSUPPORTED));
    CHECK(unknown.closed() && unknown.request_count() == 1U);

    FakeTransport zero_timeout;
    const auto invalid = w::fetch_status(
        zero_timeout, dtv::userland::Timeout{0U}, w::kFeatureCardService);
    CHECK(!invalid && invalid.error == Error::INVALID_ARGUMENT);
    CHECK(zero_timeout.closed() && zero_timeout.request_count() == 0U);
}

void test_lnb_power_client_success_and_failures()
{
    FakeTransport on;
    on.advance_per_read(1U);
    const auto enabled = w::set_lnb_power(on, true, dtv::userland::Timeout{1000U});
    CHECK(enabled && enabled.response.enabled);
    CHECK(enabled.response.epoch == make_server().epoch);
    CHECK(!on.closed() && on.last_lnb_enabled() && on.request_count() == 4U);
    CHECK(on.request_id(3U) == 4U && on.request_opcode(3U) == w::kOpcodeLnbPower);
    CHECK(on.write_timeout(3U) < 1000U);

    FakeTransport off;
    const auto disabled = w::set_lnb_power(off, false, dtv::userland::Timeout{1000U});
    CHECK(disabled && !disabled.response.enabled);
    CHECK(!off.closed() && !off.last_lnb_enabled() && off.request_count() == 4U);
    CHECK(off.request_id(3U) == 4U && off.request_opcode(3U) == w::kOpcodeLnbPower);

    FakeTransport zero_timeout;
    const auto invalid = w::set_lnb_power(
        zero_timeout, true, dtv::userland::Timeout{0U});
    CHECK(!invalid && invalid.error == Error::INVALID_ARGUMENT);
    CHECK(zero_timeout.closed() && zero_timeout.request_count() == 0U);

    FakeTransport remote(Fault::lnb_remote_error);
    const auto remote_result = w::set_lnb_power(
        remote, true, dtv::userland::Timeout{1000U});
    CHECK(!remote_result && remote_result.error == Error::UNSUPPORTED);
    CHECK(remote_result.remote_error_code == static_cast<std::uint32_t>(Error::UNSUPPORTED));
    CHECK(remote.closed() && remote.request_count() == 4U);

    const Fault faults[] = {Fault::lnb_eof, Fault::timeout_lnb, Fault::bad_lnb_epoch,
                            Fault::bad_lnb_enabled, Fault::truncated_lnb};
    const Error errors[] = {Error::DISCONNECTED, Error::TIMEOUT, Error::PROTOCOL_ERROR,
                            Error::PROTOCOL_ERROR, Error::PROTOCOL_ERROR};
    for (std::size_t index = 0U; index < sizeof(faults) / sizeof(faults[0]); ++index) {
        FakeTransport failed(faults[index]);
        const auto failed_result = w::set_lnb_power(
            failed, false, dtv::userland::Timeout{1000U});
        CHECK(!failed_result && failed_result.error == errors[index]);
        CHECK(failed.closed() && failed.request_count() == 4U);
    }
}

void test_card_session_status_and_disconnect()
{
    FakeTransport missing_feature;
    w::CardClientOptions options{};
    options.reader_id = 23U;
    const auto unsupported = w::begin_card(
        missing_feature, options, dtv::userland::Timeout{1000U});
    CHECK(!unsupported && unsupported.error == Error::UNSUPPORTED);
    CHECK(unsupported.remote_error_code == static_cast<std::uint32_t>(Error::UNSUPPORTED));
    CHECK(missing_feature.closed() && missing_feature.request_count() == 1U);

    FakeTransport no_reader(Fault::none, false, w::kFeatureCardService, false);
    const auto absent = w::begin_card(no_reader, options, dtv::userland::Timeout{1000U});
    CHECK(!absent && absent.error == Error::NOT_FOUND);
    CHECK(no_reader.closed() && no_reader.request_count() == 3U);

    FakeTransport transport(Fault::none, false, w::kFeatureCardService, true);
    transport.advance_per_read(1U);
    options.expected_product = ByteView{
        reinterpret_cast<const std::uint8_t*>("test-product"), 12U};
    const auto opened = w::begin_card(
        transport, options, dtv::userland::Timeout{5000U});
    CHECK(opened && opened.error == Error::OK);
    CHECK(opened.session.epoch == make_server(w::kFeatureCardService).epoch);
    CHECK(opened.session.reader_id == 23U && opened.session.card_handle == 0x1234U);
    CHECK(opened.session.bound_generation == 9U && opened.session.next_request_id == 6U);
    CHECK(opened.session.protocol.present &&
          opened.session.protocol.value == w::CardProtocol::t1);
    CHECK(opened.reader_status.key.reader_id == 23U && opened.connection.usable);
    CHECK(!transport.closed() && transport.request_count() == 5U);
    CHECK(transport.request_id(0U) == 1U && transport.request_id(1U) == 2U &&
          transport.request_id(2U) == 3U && transport.request_id(3U) == 4U &&
          transport.request_id(4U) == 5U);
    CHECK(transport.request_opcode(3U) == w::kOpcodeCardStatus &&
          transport.request_opcode(4U) == w::kOpcodeCardConnect);
    CHECK(transport.write_timeout(3U) == 5000U &&
          transport.write_timeout(4U) < transport.write_timeout(3U));

    auto session = opened.session;
    const auto status = w::card_status(
        transport, session, dtv::userland::Timeout{1000U});
    CHECK(status && status.response.key.epoch == session.epoch);
    CHECK(transport.request_id(5U) == 6U &&
          transport.request_opcode(5U) == w::kOpcodeCardStatus);

    const auto disconnected = w::card_disconnect(
        transport, session, w::Disposition::leave, dtv::userland::Timeout{1000U});
    CHECK(disconnected && disconnected.response.consumed);
    CHECK(transport.request_id(6U) == 7U &&
          transport.request_opcode(6U) == w::kOpcodeCardDisconnect);
    CHECK(session.card_handle == 0U && session.bound_generation == 0U);
    CHECK(!session.protocol.present);
    CHECK(!transport.closed());
}

void test_card_reset_and_transmit()
{
    FakeTransport transport(Fault::none, false, w::kFeatureCardService, true);
    w::CardClientOptions options{};
    options.reader_id = 23U;
    const auto opened = w::begin_card(
        transport, options, dtv::userland::Timeout{1000U});
    CHECK(opened);
    auto session = opened.session;
    const auto reset = w::card_reset(transport, session, dtv::userland::Timeout{1000U});
    CHECK(reset && reset.response.card_handle == opened.session.card_handle);
    CHECK(reset.response.bound_generation == 10U && reset.response.current_generation == 10U);
    CHECK(reset.response.usable && reset.response.protocol.present &&
          reset.response.protocol.value == w::CardProtocol::t1);
    CHECK(reset.response.atr.present && reset.response.atr.value.length == 3U &&
          reset.response.atr.value.bytes[0U] == 0x3bU &&
          reset.response.atr.value.bytes[2U] == 0x02U);
    CHECK(session.card_handle == opened.session.card_handle &&
          session.bound_generation == 10U && session.protocol.present &&
          session.protocol.value == w::CardProtocol::t1);
    CHECK(transport.request_count() == 6U && transport.request_id(5U) == 6U &&
          transport.request_opcode(5U) == w::kOpcodeCardReset && !transport.closed());

    const std::uint8_t command[] = {0x00U, 0xa4U, 0x04U};
    const auto transmitted = w::card_transmit(
        transport, session, w::CardProtocol::t1, ByteView{command, sizeof(command)},
        dtv::userland::Timeout{1000U});
    CHECK(transmitted && transmitted.response.response.length == 2U &&
          transmitted.response.response.bytes[0U] == 0x90U &&
          transmitted.response.response.bytes[1U] == 0x00U);
    CHECK(transmitted.response.key.epoch == session.epoch &&
          transmitted.response.key.reader_id == session.reader_id &&
          transmitted.response.key.card_handle == session.card_handle &&
          transmitted.response.key.bound_generation == session.bound_generation);
    CHECK(transport.request_count() == 7U && transport.request_id(6U) == 7U &&
          transport.request_opcode(6U) == w::kOpcodeCardTransmit && !transport.closed());
    CHECK(session.card_handle == opened.session.card_handle &&
          session.bound_generation == 10U && session.protocol.present);

    FakeTransport unusable(Fault::card_reset_unusable, false,
                           w::kFeatureCardService, true);
    const auto unusable_open = w::begin_card(
        unusable, options, dtv::userland::Timeout{1000U});
    CHECK(unusable_open);
    auto unusable_session = unusable_open.session;
    const auto unusable_reset = w::card_reset(
        unusable, unusable_session, dtv::userland::Timeout{1000U});
    CHECK(unusable_reset && !unusable_reset.response.usable &&
          !unusable_reset.response.protocol.present && !unusable_reset.response.atr.present);
    CHECK(unusable_session.bound_generation == 10U &&
          !unusable_session.protocol.present && !unusable.closed() &&
          unusable.request_count() == 6U &&
          unusable.request_opcode(5U) == w::kOpcodeCardReset);
    const auto unusable_transmit = w::card_transmit(
        unusable, unusable_session, w::CardProtocol::t1,
        ByteView{command, sizeof(command)}, dtv::userland::Timeout{1000U});
    CHECK(!unusable_transmit && unusable_transmit.error == Error::NOT_READY &&
          unusable.request_count() == 6U && unusable.closed());

    FakeTransport maximum(Fault::card_transmit_maximum, false,
                          w::kFeatureCardService, true);
    const auto maximum_open = w::begin_card(
        maximum, options, dtv::userland::Timeout{1000U});
    CHECK(maximum_open);
    std::array<std::uint8_t, w::kMaxApduBytes> maximum_command{};
    maximum_command[0U] = 0x00U;
    auto maximum_session = maximum_open.session;
    const auto maximum_result = w::card_transmit(
        maximum, maximum_session, w::CardProtocol::t1,
        ByteView{maximum_command.data(), maximum_command.size()},
        dtv::userland::Timeout{1000U});
    CHECK(maximum_result &&
          maximum_result.response.response.length == w::kMaxApduBytes);
    CHECK(maximum.request_count() == 6U && maximum.request_id(5U) == 6U &&
          maximum.request_opcode(5U) == w::kOpcodeCardTransmit && !maximum.closed());

    const Fault reset_faults[] = {Fault::card_reset_remote_error,
                                  Fault::card_reset_bad_handle,
                                  Fault::card_reset_bad_generation,
                                  Fault::card_reset_eof, Fault::truncated_card_reset,
                                  Fault::timeout_card_reset};
    const Error reset_errors[] = {Error::CARD_REMOVED, Error::PROTOCOL_ERROR,
                                  Error::PROTOCOL_ERROR, Error::DISCONNECTED,
                                  Error::PROTOCOL_ERROR, Error::TIMEOUT};
    for (std::size_t index = 0U;
         index < sizeof(reset_faults) / sizeof(reset_faults[0]); ++index) {
        FakeTransport failed(reset_faults[index], false, w::kFeatureCardService, true);
        const auto card = w::begin_card(
            failed, options, dtv::userland::Timeout{1000U});
        CHECK(card);
        auto failed_session = card.session;
        const auto result = w::card_reset(
            failed, failed_session, dtv::userland::Timeout{1000U});
        CHECK(!result && result.error == reset_errors[index]);
        CHECK(failed_session.card_handle == card.session.card_handle &&
              failed_session.bound_generation == card.session.bound_generation &&
              failed_session.protocol.present == card.session.protocol.present &&
              failed_session.protocol.value == card.session.protocol.value &&
              failed_session.next_request_id == 7U);
        CHECK(result.response.epoch == w::Uuid{} && result.response.card_handle == 0U);
        CHECK(failed.closed() && failed.request_count() == 6U &&
              failed.request_id(5U) == 6U &&
              failed.request_opcode(5U) == w::kOpcodeCardReset);
        CHECK(reset_faults[index] != Fault::card_reset_remote_error ||
              result.remote_error_code == static_cast<std::uint32_t>(Error::CARD_REMOVED));
    }

    const Fault transmit_faults[] = {Fault::card_transmit_remote_error,
                                     Fault::card_transmit_bad_key,
                                     Fault::card_transmit_eof,
                                     Fault::truncated_card_transmit,
                                     Fault::timeout_card_transmit};
    const Error transmit_errors[] = {Error::CARD_REMOVED, Error::PROTOCOL_ERROR,
                                     Error::DISCONNECTED, Error::PROTOCOL_ERROR,
                                     Error::TIMEOUT};
    for (std::size_t index = 0U;
         index < sizeof(transmit_faults) / sizeof(transmit_faults[0]); ++index) {
        FakeTransport failed(transmit_faults[index], false,
                             w::kFeatureCardService, true);
        const auto card = w::begin_card(
            failed, options, dtv::userland::Timeout{1000U});
        CHECK(card);
        auto failed_session = card.session;
        const auto result = w::card_transmit(
            failed, failed_session, w::CardProtocol::t1,
            ByteView{command, sizeof(command)}, dtv::userland::Timeout{1000U});
        CHECK(!result && result.error == transmit_errors[index]);
        CHECK(result.response.response.length == 0U);
        CHECK(result.response.key.epoch == w::Uuid{});
        CHECK(failed_session.card_handle == card.session.card_handle &&
              failed_session.bound_generation == card.session.bound_generation &&
              failed_session.protocol.present == card.session.protocol.present &&
              failed_session.protocol.value == card.session.protocol.value &&
              failed_session.next_request_id == 7U);
        CHECK(failed.closed() && failed.request_count() == 6U &&
              failed.request_id(5U) == 6U &&
              failed.request_opcode(5U) == w::kOpcodeCardTransmit);
        CHECK(transmit_faults[index] != Fault::card_transmit_remote_error ||
              result.remote_error_code == static_cast<std::uint32_t>(Error::CARD_REMOVED));
    }

    FakeTransport invalid_protocol(Fault::none, false, w::kFeatureCardService, true);
    const auto invalid_open = w::begin_card(
        invalid_protocol, options, dtv::userland::Timeout{1000U});
    CHECK(invalid_open);
    auto invalid_session = invalid_open.session;
    const auto protocol_result = w::card_transmit(
        invalid_protocol, invalid_session, static_cast<w::CardProtocol>(2U),
        ByteView{command, sizeof(command)}, dtv::userland::Timeout{1000U});
    CHECK(!protocol_result && protocol_result.error == Error::INVALID_ARGUMENT);
    CHECK(invalid_protocol.closed() && invalid_protocol.request_count() == 5U);

    FakeTransport missing_protocol(Fault::none, false, w::kFeatureCardService, true);
    const auto protocol_open = w::begin_card(
        missing_protocol, options, dtv::userland::Timeout{1000U});
    CHECK(protocol_open);
    auto no_protocol_session = protocol_open.session;
    no_protocol_session.protocol.present = false;
    const auto not_ready = w::card_transmit(
        missing_protocol, no_protocol_session, w::CardProtocol::t1,
        ByteView{command, sizeof(command)}, dtv::userland::Timeout{1000U});
    CHECK(!not_ready && not_ready.error == Error::NOT_READY);
    CHECK(missing_protocol.closed() && missing_protocol.request_count() == 5U);

    FakeTransport empty_command(Fault::none, false, w::kFeatureCardService, true);
    const auto empty_open = w::begin_card(
        empty_command, options, dtv::userland::Timeout{1000U});
    CHECK(empty_open);
    auto empty_session = empty_open.session;
    const auto empty_result = w::card_transmit(
        empty_command, empty_session, w::CardProtocol::t1, ByteView{},
        dtv::userland::Timeout{1000U});
    CHECK(!empty_result && empty_result.error == Error::INVALID_ARGUMENT);
    CHECK(empty_command.closed() && empty_command.request_count() == 5U);

    FakeTransport invalid_length(Fault::none, false, w::kFeatureCardService, true);
    const auto length_open = w::begin_card(
        invalid_length, options, dtv::userland::Timeout{1000U});
    CHECK(length_open);
    auto length_session = length_open.session;
    std::array<std::uint8_t, w::kMaxApduBytes + 1U> oversized{};
    const auto length_result = w::card_transmit(
        invalid_length, length_session, w::CardProtocol::t1,
        ByteView{oversized.data(), oversized.size()}, dtv::userland::Timeout{1000U});
    CHECK(!length_result && length_result.error == Error::INVALID_ARGUMENT);
    CHECK(invalid_length.closed() && invalid_length.request_count() == 5U);
}

void test_card_session_failures_close_without_retry()
{
    const Fault faults[] = {Fault::card_status_remote_error, Fault::card_connect_remote_error,
                            Fault::card_eof,
                            Fault::timeout_card, Fault::bad_card_status_epoch,
                            Fault::bad_card_connect_epoch, Fault::bad_card_connect_generation,
                            Fault::truncated_card_connect};
    const Error errors[] = {Error::UNSUPPORTED, Error::NO_CARD, Error::DISCONNECTED,
                            Error::TIMEOUT, Error::PROTOCOL_ERROR, Error::PROTOCOL_ERROR,
                            Error::PROTOCOL_ERROR, Error::PROTOCOL_ERROR};
    for (std::size_t index = 0U; index < sizeof(faults) / sizeof(faults[0]); ++index) {
        FakeTransport transport(faults[index], false, w::kFeatureCardService, true);
        w::CardClientOptions options{};
        options.reader_id = 23U;
        const auto result = w::begin_card(
            transport, options, dtv::userland::Timeout{1000U});
        CHECK(!result && result.error == errors[index]);
        CHECK(transport.closed());
        CHECK(result.session.epoch == w::Uuid{} && result.session.card_handle == 0U);
        CHECK(result.reader_status.key.epoch == w::Uuid{} &&
              result.connection.card_handle == 0U);
        CHECK(transport.request_count() ==
              ((faults[index] == Fault::bad_card_status_epoch ||
                faults[index] == Fault::card_status_remote_error) ? 4U : 5U));
        CHECK(faults[index] != Fault::card_status_remote_error ||
              result.remote_error_code == static_cast<std::uint32_t>(Error::UNSUPPORTED));
        CHECK(faults[index] != Fault::card_connect_remote_error ||
              result.remote_error_code == static_cast<std::uint32_t>(Error::NO_CARD));
    }

    FakeTransport wrong_identity(Fault::none, false, w::kFeatureCardService, true);
    w::CardClientOptions options{};
    options.reader_id = 23U;
    options.expected_product = ByteView{reinterpret_cast<const std::uint8_t*>("other"), 5U};
    const auto not_found = w::begin_card(
        wrong_identity, options, dtv::userland::Timeout{1000U});
    CHECK(!not_found && not_found.error == Error::NOT_FOUND);
    CHECK(wrong_identity.closed() && wrong_identity.request_count() == 3U);

    FakeTransport stale(Fault::card_disconnect_remote_error, false,
                        w::kFeatureCardService, true);
    options.expected_product = ByteView{};
    const auto opened = w::begin_card(stale, options, dtv::userland::Timeout{1000U});
    CHECK(opened);
    auto session = opened.session;
    const auto rejected = w::card_disconnect(
        stale, session, w::Disposition::leave, dtv::userland::Timeout{1000U});
    CHECK(!rejected && rejected.error == Error::PROTOCOL_ERROR);
    CHECK(rejected.remote_error_code == w::kWireErrorStaleHandle);
    CHECK(session.card_handle == opened.session.card_handle && stale.closed());
    CHECK(stale.request_count() == 6U);

    const Fault disconnect_faults[] = {Fault::bad_disconnect_key, Fault::bad_disconnect_handle,
                                       Fault::card_not_consumed,
                                       Fault::card_disconnect_unquiesced_without_error,
                                       Fault::card_disconnect_eof};
    const Error disconnect_errors[] = {Error::PROTOCOL_ERROR, Error::PROTOCOL_ERROR,
                                       Error::PROTOCOL_ERROR, Error::PROTOCOL_ERROR,
                                       Error::DISCONNECTED};
    for (std::size_t index = 0U;
         index < sizeof(disconnect_faults) / sizeof(disconnect_faults[0]); ++index) {
        FakeTransport transport(disconnect_faults[index], false,
                                w::kFeatureCardService, true);
        const auto card = w::begin_card(
            transport, options, dtv::userland::Timeout{1000U});
        CHECK(card);
        auto card_session = card.session;
        const auto result = w::card_disconnect(
            transport, card_session, w::Disposition::leave,
            dtv::userland::Timeout{1000U});
        CHECK(!result && result.error == disconnect_errors[index]);
        if (disconnect_faults[index] == Fault::card_disconnect_unquiesced_without_error) {
            CHECK(result.response.consumed && !result.response.quiesced &&
                  !result.response.error.present);
            CHECK(card_session.card_handle == 0U && card_session.bound_generation == 0U &&
                  !card_session.protocol.present);
        } else {
            CHECK(card_session.card_handle == card.session.card_handle);
        }
        CHECK(transport.closed() && transport.request_count() == 6U);
    }

    FakeTransport unquiesced(Fault::card_disconnect_unquiesced_error, false,
                             w::kFeatureCardService, true);
    const auto card = w::begin_card(
        unquiesced, options, dtv::userland::Timeout{1000U});
    CHECK(card);
    auto card_session = card.session;
    const auto cleanup_failed = w::card_disconnect(
        unquiesced, card_session, w::Disposition::leave,
        dtv::userland::Timeout{1000U});
    CHECK(!cleanup_failed && cleanup_failed.error == Error::USB_IO);
    CHECK(cleanup_failed.remote_error_code == static_cast<std::uint32_t>(Error::USB_IO));
    CHECK(cleanup_failed.response.consumed && !cleanup_failed.response.quiesced &&
          cleanup_failed.response.error.present &&
          cleanup_failed.response.error.value.code == cleanup_failed.remote_error_code);
    CHECK(card_session.card_handle == 0U && card_session.bound_generation == 0U &&
          !card_session.protocol.present);
    CHECK(unquiesced.closed() && unquiesced.request_count() == 6U &&
          unquiesced.request_id(5U) == 6U);
}

void test_begin_recording_validator_and_two_budgets()
{
    FakeTransport transport;
    transport.advance_per_read(1U);
    w::RecordingOptions options{};
    options.receiver_id = 513U;
    options.tune.frequency_hz = 473142857U;
    options.tune.timeout_ms = 1000U;
    w::Uuid expected_epoch{};
    for (std::size_t index = 0U; index < expected_epoch.size(); ++index)
        expected_epoch[index] = static_cast<std::uint8_t>(0xa0U + index);
    const auto result = w::begin_recording(
        transport, options, dtv::userland::Timeout{1000U},
        accept_status_epoch, &expected_epoch);
    CHECK(result);
    CHECK(!transport.closed());
    CHECK(result.session.epoch == expected_epoch);
    CHECK(result.session.lease_id == 77U && result.session.tune_revision == 3U);
    CHECK(result.session.tune_result.requested_hz == options.tune.frequency_hz);
    CHECK(result.session.stream_id == 99U && result.session.attach_token[0] == 0x33U);
    CHECK(result.session.expires_after_ms == 1000U && result.session.next_request_id == 7U);
    CHECK(transport.request_count() == 6U);
    CHECK(transport.request_id(0U) == 1U && transport.request_id(1U) == 2U &&
          transport.request_id(2U) == 3U && transport.request_id(3U) == 4U &&
          transport.request_id(4U) == 5U && transport.request_id(5U) == 6U);
    CHECK(transport.request_opcode(0U) == w::kOpcodeHello &&
          transport.request_opcode(1U) == w::kOpcodeList &&
          transport.request_opcode(2U) == w::kOpcodeStatus &&
          transport.request_opcode(3U) == w::kOpcodeAcquire &&
          transport.request_opcode(4U) == w::kOpcodeTune &&
          transport.request_opcode(5U) == w::kOpcodeStartStream);
    CHECK(transport.write_timeout(0U) == 1000U);
    CHECK(transport.write_timeout(3U) == 1000U);

    w::RecordingSession session = result.session;
    const auto stopped = w::stop_recording(
        transport, session, dtv::userland::Timeout{100U});
    CHECK(stopped && stopped.response.final.id == result.session.stream_id);
    CHECK(session.next_request_id == 8U);
    const auto released = w::release_recording(
        transport, session, dtv::userland::Timeout{100U});
    CHECK(released && released.response.released);
    CHECK(session.next_request_id == 9U && !transport.closed());
    CHECK(transport.request_count() == 8U);
    CHECK(transport.request_id(6U) == 7U && transport.request_id(7U) == 8U);
    CHECK(transport.request_opcode(6U) == w::kOpcodeStopStream &&
          transport.request_opcode(7U) == w::kOpcodeRelease);
}

void test_recording_pid_buffers_accept_limits_and_reject_invalid_values()
{
    w::RecordingOptions ten_options{};
    ten_options.tune.frequency_hz = 473142857U;
    ten_options.tune.timeout_ms = 1000U;
    ten_options.tune.pids.present = true;
    ten_options.tune.pids.value.count = 10U;
    for (std::size_t index = 0U; index < 10U; ++index)
        ten_options.tune.pids.value.items[index] = static_cast<std::uint16_t>(index);
    FakeTransport ten(Fault::none, true);
    const auto ten_result = w::begin_recording(
        ten, ten_options, dtv::userland::Timeout{100U});
    CHECK(ten_result && ten.request_count() == 6U);
    CHECK(ten.tune_pid_count() == 10U && ten.tune_pid_first() == 0U &&
          ten.tune_pid_last() == 9U);

    w::RecordingOptions max_options{};
    max_options.tune.frequency_hz = 473142857U;
    max_options.tune.timeout_ms = 1000U;
    max_options.tune.pids.present = true;
    max_options.tune.pids.value.count = w::kMaxPids;
    for (std::size_t index = 0U; index < w::kMaxPids; ++index)
        max_options.tune.pids.value.items[index] = static_cast<std::uint16_t>(index);
    FakeTransport maximum(Fault::none, true);
    const auto max_result = w::begin_recording(
        maximum, max_options, dtv::userland::Timeout{100U});
    CHECK(max_result && maximum.request_count() == 6U);
    CHECK(maximum.tune_pid_count() == w::kMaxPids && maximum.tune_pid_first() == 0U &&
          maximum.tune_pid_last() == 8191U);

    max_options.tune.pids.value.items[1U] = max_options.tune.pids.value.items[0U];
    FakeTransport invalid(Fault::none, true);
    const auto invalid_result = w::begin_recording(
        invalid, max_options, dtv::userland::Timeout{100U});
    CHECK(!invalid_result && invalid_result.error == Error::INVALID_ARGUMENT);
    CHECK(invalid.closed() && invalid.request_count() == 4U);
    CHECK(invalid.request_opcode(3U) == w::kOpcodeAcquire);
}

void test_release_remote_error_is_preserved()
{
    w::RecordingOptions options{};
    options.tune.frequency_hz = 473142857U;
    options.tune.timeout_ms = 1000U;
    FakeTransport remote(Fault::release_remote_error);
    const auto begun = w::begin_recording(
        remote, options, dtv::userland::Timeout{100U});
    CHECK(begun);
    w::RecordingSession session = begun.session;
    const auto released = w::release_recording(
        remote, session, dtv::userland::Timeout{100U});
    CHECK(!released && released.error == Error::PROTOCOL_ERROR);
    CHECK(released.remote_error_code == 999U);
    CHECK(!released.response.released && released.response.error.present &&
          released.response.error.value.code == 999U);
    CHECK(remote.closed() && remote.request_count() == 7U && session.next_request_id == 8U);

    FakeTransport wire_error(Fault::release_wire_error);
    const auto wire_begun = w::begin_recording(
        wire_error, options, dtv::userland::Timeout{100U});
    CHECK(wire_begun);
    w::RecordingSession wire_session = wire_begun.session;
    const auto wire_result = w::release_recording(
        wire_error, wire_session, dtv::userland::Timeout{100U});
    CHECK(!wire_result && wire_result.error == Error::PROTOCOL_ERROR);
    CHECK(wire_result.remote_error_code == 999U &&
          wire_result.response.epoch == w::Uuid{});
    CHECK(wire_error.closed() && wire_error.request_count() == 7U);

    FakeTransport malformed(Fault::release_without_error);
    const auto malformed_begun = w::begin_recording(
        malformed, options, dtv::userland::Timeout{100U});
    CHECK(malformed_begun);
    w::RecordingSession malformed_session = malformed_begun.session;
    const auto malformed_result = w::release_recording(
        malformed, malformed_session, dtv::userland::Timeout{100U});
    CHECK(!malformed_result && malformed_result.error == Error::PROTOCOL_ERROR);
    CHECK(!malformed_result.response.released && !malformed_result.response.error.present);
    CHECK(malformed.closed());

    FakeTransport wrong_epoch(Fault::bad_release_epoch);
    const auto epoch_begun = w::begin_recording(
        wrong_epoch, options, dtv::userland::Timeout{100U});
    CHECK(epoch_begun);
    w::RecordingSession epoch_session = epoch_begun.session;
    const auto epoch_result = w::release_recording(
        wrong_epoch, epoch_session, dtv::userland::Timeout{100U});
    CHECK(!epoch_result && epoch_result.error == Error::PROTOCOL_ERROR);
    CHECK(epoch_result.response.epoch != epoch_session.epoch && wrong_epoch.closed());
}

void test_recording_validator_rejection_and_timeout_do_not_publish_session()
{
    FakeTransport rejected;
    w::RecordingOptions options{};
    const auto rejected_result = w::begin_recording(
        rejected, options, dtv::userland::Timeout{100U}, reject_status, nullptr);
    CHECK(!rejected_result && rejected_result.error == Error::NOT_FOUND);
    CHECK(rejected.closed() && rejected.request_count() == 3U);
    CHECK(rejected_result.session.epoch == w::Uuid{});

    FakeTransport timed_out(Fault::timeout_start);
    options.tune.frequency_hz = 473142857U;
    options.tune.timeout_ms = 1000U;
    const auto timeout_result = w::begin_recording(
        timed_out, options, dtv::userland::Timeout{100U});
    CHECK(!timeout_result && timeout_result.error == Error::TIMEOUT);
    CHECK(timed_out.closed() && timed_out.request_count() == 6U);
    CHECK(timeout_result.session.epoch == w::Uuid{});
}

void test_recording_remote_error_and_cleanup_timeouts_are_not_retried()
{
    w::RecordingOptions options{};
    options.tune.frequency_hz = 473142857U;
    options.tune.timeout_ms = 1000U;
    FakeTransport remote(Fault::remote_tune_error);
    const auto remote_result = w::begin_recording(
        remote, options, dtv::userland::Timeout{100U});
    CHECK(!remote_result && remote_result.error == Error::PROTOCOL_ERROR);
    CHECK(remote_result.remote_error_code == 999U);
    CHECK(remote.closed() && remote.request_count() == 5U);
    CHECK(remote.request_id(4U) == 5U && remote.request_opcode(4U) == w::kOpcodeTune);
    CHECK(remote_result.session.epoch == w::Uuid{});

    FakeTransport stop_timeout(Fault::timeout_stop);
    const auto begun = w::begin_recording(
        stop_timeout, options, dtv::userland::Timeout{100U});
    CHECK(begun);
    w::RecordingSession stop_session = begun.session;
    const auto stopped = w::stop_recording(
        stop_timeout, stop_session, dtv::userland::Timeout{100U});
    CHECK(!stopped && stopped.error == Error::TIMEOUT);
    CHECK(stop_timeout.closed() && stop_timeout.request_count() == 7U);
    CHECK(stop_session.next_request_id == 8U);

    FakeTransport release_timeout(Fault::timeout_release);
    const auto second_begun = w::begin_recording(
        release_timeout, options, dtv::userland::Timeout{100U});
    CHECK(second_begun);
    w::RecordingSession release_session = second_begun.session;
    const auto second_stopped = w::stop_recording(
        release_timeout, release_session, dtv::userland::Timeout{100U});
    CHECK(second_stopped);
    const auto released = w::release_recording(
        release_timeout, release_session, dtv::userland::Timeout{100U});
    CHECK(!released && released.error == Error::TIMEOUT);
    CHECK(release_timeout.closed() && release_timeout.request_count() == 8U);
    CHECK(release_session.next_request_id == 9U);
}

void test_timed_status_timeout_closes_without_retry()
{
    FakeTransport transport(Fault::timeout_list);
    const auto result = w::fetch_status(transport, dtv::userland::Timeout{100U});
    CHECK(!result && result.error == Error::TIMEOUT);
    CHECK(transport.closed() && transport.request_count() == 2U);
    CHECK(result.snapshot.list.epoch == w::Uuid{});

    FakeTransport trickled;
    trickled.advance_per_read(50U);
    const auto expired = w::fetch_status(trickled, dtv::userland::Timeout{100U});
    CHECK(!expired && expired.error == Error::TIMEOUT);
    CHECK(trickled.closed() && trickled.request_count() == 1U);
}

#if defined(DTV_USERLAND_CONTROL_STACK_PROBES)
class ReplayTransport final : public w::ClientTransport {
public:
    explicit ReplayTransport(const FakeTransport& source) noexcept : source_(source) {}

    Result<void> write_all(ByteView bytes) noexcept override
    {
        if (closed_ || reply_index_ != request_count_ || request_count_ >= 3U) {
            return Result<void>::failure(Error::PROTOCOL_ERROR);
        }
        const auto frame = w::decode_frame(bytes);
        if (!frame || frame.value().header.kind != w::Kind::request ||
            frame.value().header.request_id != request_count_ + 1U ||
            frame.value().header.opcode != expected_opcodes_[request_count_]) {
            return Result<void>::failure(Error::PROTOCOL_ERROR);
        }
        reply_offset_ = 0U;
        ++request_count_;
        return Result<void>::success();
    }

    Result<std::size_t> read_some(MutableByteView bytes) noexcept override
    {
        const std::size_t index = request_count_ - 1U;
        const std::size_t size = source_.recorded_size(index);
        if (reply_offset_ == size) {
            return Result<std::size_t>::success(0U);
        }
        const std::size_t remaining = size - reply_offset_;
        const std::size_t count = remaining < bytes.size ? remaining : bytes.size;
        std::memcpy(bytes.data, source_.recorded_reply(index) + reply_offset_, count);
        reply_offset_ += count;
        if (reply_offset_ == size) ++reply_index_;
        return Result<std::size_t>::success(count);
    }

    void close() noexcept override { closed_ = true; }
    bool closed() const noexcept { return closed_; }
    std::size_t request_count() const noexcept { return request_count_; }

private:
    const FakeTransport& source_;
    const std::uint16_t expected_opcodes_[3U] = {w::kOpcodeHello, w::kOpcodeList,
                                                  w::kOpcodeStatus};
    std::size_t request_count_ = 0U;
    std::size_t reply_index_ = 0U;
    std::size_t reply_offset_ = 0U;
    bool closed_ = false;
};

class EofTransport final : public w::ClientTransport {
public:
    Result<void> write_all(ByteView) noexcept override { return Result<void>::success(); }
    Result<std::size_t> read_some(MutableByteView) noexcept override
    {
        return Result<std::size_t>::success(0U);
    }
    void close() noexcept override { closed_ = true; }
    bool closed() const noexcept { return closed_; }

private:
    bool closed_ = false;
};

class TimedEofTransport final : public w::TimedClientTransport {
public:
    Result<void> write_all(ByteView, dtv::userland::Timeout) noexcept override
    {
        return Result<void>::success();
    }
    Result<std::size_t> read_some(MutableByteView, dtv::userland::Timeout) noexcept override
    {
        return Result<std::size_t>::success(0U);
    }
    std::uint64_t monotonic_ms() noexcept override { return 0U; }
    void close() noexcept override { closed_ = true; }
    bool closed() const noexcept { return closed_; }

private:
    bool closed_ = false;
};

struct StackProbe final {
    w::ClientTransport* transport;
    Error result = Error::INTERNAL;
    bool snapshot_present = false;
};

struct RecordingStackProbe final {
    w::TimedClientTransport* transport;
    const w::RecordingOptions* options;
    Error result = Error::INTERNAL;
    bool began = false;
    bool stopped = false;
    bool released = false;
};

struct TimedEofStackProbe final {
    w::TimedClientTransport* transport;
    Error result = Error::INTERNAL;
};

struct LnbStackProbe final {
    w::TimedClientTransport* transport;
    Error result = Error::INTERNAL;
    bool enabled = false;
};

struct CardOpenStackProbe final {
    w::TimedClientTransport* transport;
    Error result = Error::INTERNAL;
    bool opened = false;
};

struct CardTransmitStackProbe final {
    w::TimedClientTransport* transport;
    w::CardClientSession session;
    std::array<std::uint8_t, w::kMaxApduBytes> command{};
    Error result = Error::INTERNAL;
    std::size_t response_length = 0U;
};

void* fetch_on_thread(void* value) noexcept
{
    auto& probe = *static_cast<StackProbe*>(value);
    const w::StatusFetchResult result = w::fetch_status(*probe.transport);
    probe.result = result.error;
    probe.snapshot_present = result.snapshot.list.epoch != w::Uuid{};
    return nullptr;
}

bool run_stack_probe(w::ClientTransport& transport, std::size_t stack_size,
                     Error expected_error, bool expect_snapshot)
{
    StackProbe probe{&transport};
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0) return false;
    const int set_result = pthread_attr_setstacksize(&attributes, stack_size);
    pthread_t thread{};
    const int create_result = set_result == 0
                                  ? pthread_create(&thread, &attributes, fetch_on_thread, &probe)
                                  : set_result;
    pthread_attr_destroy(&attributes);
    if (create_result != 0) return false;
    if (pthread_join(thread, nullptr) != 0) return false;
    return probe.result == expected_error && probe.snapshot_present == expect_snapshot;
}

void* recording_on_thread(void* value) noexcept
{
    auto& probe = *static_cast<RecordingStackProbe*>(value);
    const auto begun = w::begin_recording(*probe.transport, *probe.options,
                                          dtv::userland::Timeout{1000U});
    probe.began = static_cast<bool>(begun);
    if (!begun) {
        probe.result = begun.error;
        return nullptr;
    }
    w::RecordingSession session = begun.session;
    const auto stopped = w::stop_recording(*probe.transport, session,
                                           dtv::userland::Timeout{1000U});
    probe.stopped = static_cast<bool>(stopped);
    if (!stopped) {
        probe.result = stopped.error;
        return nullptr;
    }
    const auto released = w::release_recording(*probe.transport, session,
                                               dtv::userland::Timeout{1000U});
    probe.released = static_cast<bool>(released);
    probe.result = released.error;
    return nullptr;
}

void* timed_eof_on_thread(void* value) noexcept
{
    auto& probe = *static_cast<TimedEofStackProbe*>(value);
    const auto result = w::fetch_status(*probe.transport, dtv::userland::Timeout{1000U});
    probe.result = result.error;
    return nullptr;
}

void* lnb_power_on_thread(void* value) noexcept
{
    auto& probe = *static_cast<LnbStackProbe*>(value);
    const auto result = w::set_lnb_power(*probe.transport, true,
                                         dtv::userland::Timeout{1000U});
    probe.result = result.error;
    probe.enabled = result && result.response.enabled;
    return nullptr;
}

void* card_open_on_thread(void* value) noexcept
{
    auto& probe = *static_cast<CardOpenStackProbe*>(value);
    w::CardClientOptions options{};
    options.reader_id = 23U;
    const auto result = w::begin_card(*probe.transport, options,
                                      dtv::userland::Timeout{1000U});
    probe.result = result.error;
    probe.opened = result && result.session.card_handle != 0U;
    return nullptr;
}

void* card_transmit_on_thread(void* value) noexcept
{
    auto& probe = *static_cast<CardTransmitStackProbe*>(value);
    const auto result = w::card_transmit(
        *probe.transport, probe.session, w::CardProtocol::t1,
        ByteView{probe.command.data(), probe.command.size()},
        dtv::userland::Timeout{1000U});
    probe.result = result.error;
    probe.response_length = result.response.response.length;
    return nullptr;
}

bool run_timed_eof_stack_probe(TimedEofStackProbe& probe, std::size_t stack_size)
{
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0) return false;
    const int set_result = pthread_attr_setstacksize(&attributes, stack_size);
    pthread_t thread{};
    const int create_result = set_result == 0
                                  ? pthread_create(&thread, &attributes, timed_eof_on_thread,
                                                   &probe)
                                  : set_result;
    pthread_attr_destroy(&attributes);
    if (create_result != 0) return false;
    return pthread_join(thread, nullptr) == 0;
}

bool run_lnb_stack_probe(LnbStackProbe& probe, std::size_t stack_size)
{
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0) return false;
    const int set_result = pthread_attr_setstacksize(&attributes, stack_size);
    pthread_t thread{};
    const int create_result = set_result == 0
                                  ? pthread_create(&thread, &attributes, lnb_power_on_thread,
                                                   &probe)
                                  : set_result;
    pthread_attr_destroy(&attributes);
    if (create_result != 0) return false;
    return pthread_join(thread, nullptr) == 0;
}

bool run_card_open_stack_probe(CardOpenStackProbe& probe, std::size_t stack_size)
{
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0) return false;
    const int set_result = pthread_attr_setstacksize(&attributes, stack_size);
    pthread_t thread{};
    const int create_result = set_result == 0
                                  ? pthread_create(&thread, &attributes, card_open_on_thread,
                                                   &probe)
                                  : set_result;
    pthread_attr_destroy(&attributes);
    if (create_result != 0) return false;
    return pthread_join(thread, nullptr) == 0;
}

bool run_card_transmit_stack_probe(CardTransmitStackProbe& probe,
                                   std::size_t stack_size)
{
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0) return false;
    const int set_result = pthread_attr_setstacksize(&attributes, stack_size);
    pthread_t thread{};
    const int create_result = set_result == 0
                                  ? pthread_create(&thread, &attributes,
                                                   card_transmit_on_thread, &probe)
                                  : set_result;
    pthread_attr_destroy(&attributes);
    if (create_result != 0) return false;
    return pthread_join(thread, nullptr) == 0;
}

bool run_recording_stack_probe(RecordingStackProbe& probe, std::size_t stack_size)
{
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0) return false;
    const int set_result = pthread_attr_setstacksize(&attributes, stack_size);
    pthread_t thread{};
    const int create_result = set_result == 0
                                  ? pthread_create(&thread, &attributes, recording_on_thread,
                                                   &probe)
                                  : set_result;
    pthread_attr_destroy(&attributes);
    if (create_result != 0) return false;
    return pthread_join(thread, nullptr) == 0;
}

void test_small_stack_probes()
{
#if defined(DTV_USERLAND_CONTROL_ADDRESS_SANITIZER)
    constexpr std::size_t kStackSizes[] = {512U * 1024U, 768U * 1024U};
#elif defined(__OPTIMIZE__)
    constexpr std::size_t kStackSizes[] = {256U * 1024U, 512U * 1024U, 768U * 1024U};
#else
    constexpr std::size_t kStackSizes[] = {512U * 1024U, 768U * 1024U};
#endif
    FakeTransport source;
    const w::StatusFetchResult seeded = w::fetch_status(source);
    CHECK(seeded);
    for (const std::size_t stack_size : kStackSizes) {
        EofTransport eof;
        CHECK(run_stack_probe(eof, stack_size, Error::DISCONNECTED, false));
        CHECK(eof.closed());

        ReplayTransport normal(source);
        CHECK(run_stack_probe(normal, stack_size, Error::OK, true));
        CHECK(!normal.closed());
        CHECK(normal.request_count() == 3U);
    }

    std::unique_ptr<TimedEofTransport> timed_eof(new (std::nothrow) TimedEofTransport{});
    CHECK(timed_eof != nullptr);
    if (timed_eof) {
        TimedEofStackProbe eof_probe{timed_eof.get()};
        CHECK(run_timed_eof_stack_probe(eof_probe, 512U * 1024U));
        CHECK(eof_probe.result == Error::DISCONNECTED && timed_eof->closed());
    }

    std::unique_ptr<FakeTransport> lnb(new (std::nothrow) FakeTransport{});
    CHECK(lnb != nullptr);
    if (lnb) {
        LnbStackProbe lnb_probe{lnb.get()};
        CHECK(run_lnb_stack_probe(lnb_probe, 512U * 1024U));
        CHECK(lnb_probe.result == Error::OK && lnb_probe.enabled);
        CHECK(!lnb->closed() && lnb->request_count() == 4U);
    }

    std::unique_ptr<FakeTransport> card(
        new (std::nothrow) FakeTransport{Fault::none, false, w::kFeatureCardService, true});
    CHECK(card != nullptr);
    if (card) {
        CardOpenStackProbe card_probe{card.get()};
        CHECK(run_card_open_stack_probe(card_probe, 512U * 1024U));
        CHECK(card_probe.result == Error::OK && card_probe.opened);
        CHECK(!card->closed() && card->request_count() == 5U);
    }

    std::unique_ptr<FakeTransport> card_transmit_transport(
        new (std::nothrow) FakeTransport{Fault::card_transmit_maximum, false,
                                         w::kFeatureCardService, true});
    CHECK(card_transmit_transport != nullptr);
    if (card_transmit_transport) {
        w::CardClientOptions options{};
        options.reader_id = 23U;
        const auto opened = w::begin_card(
            *card_transmit_transport, options, dtv::userland::Timeout{1000U});
        CHECK(opened);
        if (opened) {
            std::unique_ptr<CardTransmitStackProbe> transmit_probe(
                new (std::nothrow) CardTransmitStackProbe{card_transmit_transport.get(),
                                                          opened.session});
            CHECK(transmit_probe != nullptr);
            if (transmit_probe) {
                CHECK(run_card_transmit_stack_probe(*transmit_probe, 512U * 1024U));
                CHECK(transmit_probe->result == Error::OK &&
                      transmit_probe->response_length == w::kMaxApduBytes);
                CHECK(!card_transmit_transport->closed() &&
                      card_transmit_transport->request_count() == 6U);
            }
        }
    }

    std::unique_ptr<FakeTransport> recording(new (std::nothrow) FakeTransport{});
    CHECK(recording != nullptr);
    if (recording) {
        w::RecordingOptions options{};
        options.tune.frequency_hz = 473142857U;
        options.tune.timeout_ms = 1000U;
        RecordingStackProbe probe{recording.get(), &options};
        CHECK(run_recording_stack_probe(probe, 512U * 1024U));
        CHECK(probe.result == Error::OK && probe.began && probe.stopped && probe.released);
        CHECK(!recording->closed() && recording->request_count() == 8U);
    }
}
#endif

}  // namespace

int main()
{
    test_successful_read_only_exchange();
    test_invalid_responses_close_without_publishing_snapshot();
    test_remote_error_code_is_preserved();
    test_timed_status_uses_one_deadline_and_preserves_old_transport();
    test_timed_status_required_features();
    test_lnb_power_client_success_and_failures();
    test_card_session_status_and_disconnect();
    test_card_reset_and_transmit();
    test_card_session_failures_close_without_retry();
    test_begin_recording_validator_and_two_budgets();
    test_recording_pid_buffers_accept_limits_and_reject_invalid_values();
    test_release_remote_error_is_preserved();
    test_recording_validator_rejection_and_timeout_do_not_publish_session();
    test_recording_remote_error_and_cleanup_timeouts_are_not_retried();
    test_timed_status_timeout_closes_without_retry();
#if defined(DTV_USERLAND_CONTROL_STACK_PROBES)
    test_small_stack_probes();
#endif
    std::printf("control_client_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
