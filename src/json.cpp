// SPDX-License-Identifier: MIT
#include <dtv/userland/json.h>

#include <dtv/userland/error.h>

#include <limits>

namespace dtv::userland::wire {
namespace {

class Writer final {
public:
    explicit Writer(MutableByteView output) noexcept : output_(output) {}

    bool ch(char value) noexcept
    {
        if (size_ == std::numeric_limits<std::size_t>::max()) return false;
        if (output_.data != nullptr && size_ < output_.size) {
            output_.data[size_] = static_cast<std::uint8_t>(value);
        }
        ++size_;
        return true;
    }

    bool text(const char* value) noexcept
    {
        while (*value != '\0') {
            if (!ch(*value++)) return false;
        }
        return true;
    }

    bool string(const char* value, std::size_t length) noexcept
    {
        static constexpr char hex[] = "0123456789abcdef";
        if (!ch('"')) return false;
        for (std::size_t i = 0U; i < length; ++i) {
            const unsigned char c = static_cast<unsigned char>(value[i]);
            if (c == '"' || c == '\\') {
                if (!ch('\\') || !ch(static_cast<char>(c))) return false;
            } else if (c == '\b') {
                if (!text("\\b")) return false;
            } else if (c == '\f') {
                if (!text("\\f")) return false;
            } else if (c == '\n') {
                if (!text("\\n")) return false;
            } else if (c == '\r') {
                if (!text("\\r")) return false;
            } else if (c == '\t') {
                if (!text("\\t")) return false;
            } else if (c < 0x20U) {
                if (!text("\\u00") || !ch(hex[c >> 4U]) || !ch(hex[c & 0x0fU])) return false;
            } else if (!ch(static_cast<char>(c))) {
                return false;
            }
        }
        return ch('"');
    }

    bool number(std::uint64_t value) noexcept
    {
        char digits[20];
        std::size_t count = 0U;
        do {
            digits[count++] = static_cast<char>('0' + value % 10U);
            value /= 10U;
        } while (value != 0U);
        while (count != 0U) {
            if (!ch(digits[--count])) return false;
        }
        return true;
    }

    bool signed_number(std::int32_t value) noexcept
    {
        if (value < 0) {
            if (!ch('-')) return false;
            return number(static_cast<std::uint64_t>(-static_cast<std::int64_t>(value)));
        }
        return number(static_cast<std::uint64_t>(value));
    }

    bool quoted_u64(std::uint64_t value) noexcept
    {
        return ch('"') && number(value) && ch('"');
    }

    std::size_t size() const noexcept { return size_; }

private:
    MutableByteView output_;
    std::size_t size_ = 0U;
};

std::size_t length(const char* value) noexcept
{
    std::size_t size = 0U;
    while (value[size] != '\0') ++size;
    return size;
}

bool key(Writer& w, const char* name) noexcept
{
    return w.string(name, length(name)) && w.ch(':');
}

bool boolean(Writer& w, bool value) noexcept { return w.text(value ? "true" : "false"); }

template <std::size_t N>
bool string(Writer& w, const WString<N>& value) noexcept
{
    return w.string(value.bytes.data(), value.length);
}

bool nullable_string(Writer& w, const Optional<WString<64>>& value) noexcept
{
    return value.present ? string(w, value.value) : w.text("null");
}

const char* error_symbol(std::uint32_t code) noexcept
{
    switch (code) {
    case 0U: return "OK";
    case 1U: return "INVALID_ARGUMENT";
    case 2U: return "VERSION_MISMATCH";
    case 3U: return "NOT_FOUND";
    case 4U: return "BUSY";
    case 5U: return "NOT_READY";
    case 6U: return "TIMEOUT";
    case 7U: return "USB_IO";
    case 8U: return "DISCONNECTED";
    case 9U: return "PROTOCOL_ERROR";
    case 10U: return "FIRMWARE_REJECTED";
    case 11U: return "UNSUPPORTED";
    case 12U: return "NO_CARD";
    case 13U: return "CARD_REMOVED";
    case 14U: return "BUFFER_TOO_SMALL";
    case 15U: return "SLOW_CONSUMER";
    case 17U: return "RESOURCE_LIMIT";
    case 255U: return "INTERNAL";
    default: return nullptr;
    }
}

bool error(Writer& w, const Optional<ErrorRecord>& value) noexcept
{
    if (!value.present) return w.text("null");
    const char* symbol = error_symbol(value.value.code);
    if (!w.ch('{') || !key(w, "code") || !w.string(symbol == nullptr ? "UNKNOWN" : symbol,
                                                    symbol == nullptr ? 7U : length(symbol))) {
        return false;
    }
    if (symbol == nullptr &&
        (!w.ch(',') || !key(w, "numeric_code") || !w.number(value.value.code))) {
        return false;
    }
    return w.ch(',') && key(w, "operation") && string(w, value.value.operation) && w.ch(',') &&
           key(w, "detail") && string(w, value.value.detail) && w.ch('}');
}

bool producer(Writer& w, const Producer& value) noexcept
{
    return w.ch('{') && key(w, "product") && string(w, value.product) && w.ch(',') &&
           key(w, "version") && string(w, value.version) && w.ch(',') &&
           key(w, "core_version") && nullable_string(w, value.core_version) && w.ch('}');
}

bool model(Writer& w, const Optional<Model>& value) noexcept
{
    if (!value.present) return w.text("null");
    return string(w, value.value.name);
}

bool enclosure(Writer& w, const StatusResponse& value) noexcept
{
    if (!value.enclosure.present) return w.text("null");
    const Enclosure& v = value.enclosure.value;
    return w.ch('{') && key(w, "id") && w.number(v.id) && w.ch(',') && key(w, "model") &&
           model(w, v.model) && w.ch(',') && key(w, "serial") &&
           (v.identity.serial.present ? string(w, v.identity.serial.value) : w.text("null")) &&
           w.ch(',') && key(w, "lnb_15v_enabled") &&
           (value.lnb_15v_enabled.present ? boolean(w, value.lnb_15v_enabled.value)
                                          : w.text("null")) &&
           w.ch(',') && key(w, "card_inserted") &&
           (value.card_inserted.present ? boolean(w, value.card_inserted.value) : w.text("null")) &&
           w.ch(',') && key(w, "limits") && w.ch('{') && key(w, "max_concurrent_streams") &&
           (v.limits.max_concurrent_streams.present
                ? w.number(v.limits.max_concurrent_streams.value)
                : w.text("null")) &&
           w.text("}}");
}

const char* context_state(ContextState value) noexcept
{
    switch (value) {
    case ContextState::starting: return "starting";
    case ContextState::ready: return "ready";
    case ContextState::stopping: return "stopping";
    case ContextState::error: return "error";
    case ContextState::disconnected: return "disconnected";
    }
    return "unknown";
}

bool context(Writer& w, const Context& value) noexcept
{
    static constexpr char hex[] = "0123456789abcdef";
    if (!w.ch('{') || !key(w, "epoch") || !w.ch('"')) return false;
    for (std::uint8_t byte : value.epoch) {
        if (!w.ch(hex[(byte >> 4U) & 0x0fU]) || !w.ch(hex[byte & 0x0fU])) return false;
    }
    const char* state = context_state(value.state);
    return w.ch('"') && w.ch(',') && key(w, "instance") &&
           (value.instance.present ? string(w, value.instance.value) : w.text("null")) &&
           w.ch(',') && key(w, "state") && w.string(state, length(state)) && w.ch(',') &&
           key(w, "last_error") && error(w, value.last_error) && w.ch('}');
}

const char* scope_kind(ScopeKind value) noexcept
{
    switch (value) {
    case ScopeKind::enclosure: return "enclosure";
    case ScopeKind::receiver: return "receiver";
    case ScopeKind::reader: return "reader";
    case ScopeKind::stream: return "stream";
    }
    return "unknown";
}

bool scope(Writer& w, const Scope& value) noexcept
{
    const char* kind = scope_kind(value.kind);
    if (!w.ch('{') || !key(w, "kind") || !w.string(kind, length(kind)) || w.ch(',') == false ||
        !key(w, "id")) {
        return false;
    }
    if (value.kind == ScopeKind::enclosure) return w.text("null}");
    if (value.kind == ScopeKind::stream) return w.quoted_u64(value.id) && w.ch('}');
    return w.number(value.id) && w.ch('}');
}

const char* topology_state(TopologyState value) noexcept
{
    switch (value) {
    case TopologyState::unknown: return "unknown";
    case TopologyState::partial: return "partial";
    case TopologyState::complete: return "complete";
    }
    return "unknown";
}

bool topology(Writer& w, const Topology& value) noexcept
{
    const char* state = topology_state(value.state);
    return w.ch('{') && key(w, "state") && w.string(state, length(state)) && w.ch(',') &&
           key(w, "expected_function_count") &&
           (value.expected_function_count.present ? w.number(value.expected_function_count.value)
                                                  : w.text("null")) &&
           w.ch('}');
}

const char* mode(Mode value) noexcept
{
    switch (value) {
    case Mode::unknown: return "unknown";
    case Mode::cold: return "cold";
    case Mode::runtime: return "runtime";
    }
    return "unknown";
}

bool usb_function(Writer& w, const UsbFunction& value) noexcept
{
    static constexpr char hex[] = "0123456789abcdef";
    if (!w.ch('{') || !key(w, "id") || !w.number(value.id) || w.ch(',') == false ||
        !key(w, "usb") || !w.ch('"')) {
        return false;
    }
    for (std::size_t index = 0U; index < 2U; ++index) {
        const std::uint16_t id = index == 0U ? value.vid : value.pid;
        for (int shift = 12; shift >= 0; shift -= 4) {
            if (!w.ch(hex[(id >> shift) & 0x0fU])) return false;
        }
        if (index == 0U && !w.ch(':')) return false;
    }
    const char* function_mode = mode(value.mode);
    if (!w.ch('"') || !w.ch(',') || !key(w, "mode") ||
        !w.string(function_mode, length(function_mode)) || !w.ch(',') || !key(w, "location") ||
        !w.ch('{') || !key(w, "usb_path") ||
        !(value.location.usb_path.present ? string(w, value.location.usb_path.value) : w.text("null")) ||
        !w.ch(',') || !key(w, "bus") ||
        !(value.location.bus.present ? w.number(value.location.bus.value) : w.text("null")) ||
        !w.ch(',') || !key(w, "address") ||
        !(value.location.address.present ? w.number(value.location.address.value) : w.text("null")) ||
        !w.ch('}') || !w.ch(',') || !key(w, "serial") ||
        !(value.serial.present ? string(w, value.serial.value) : w.text("null")) ||
        !w.ch(',') || !key(w, "model") || model(w, value.model) == false || !w.ch(',') ||
        !key(w, "present") || !boolean(w, value.present) || !w.ch(',') ||
        !key(w, "last_error") || !error(w, value.last_error) || !w.ch('}')) {
        return false;
    }
    return true;
}

bool binding(Writer& w, const Optional<Binding>& value) noexcept
{
    if (!value.present) return w.text("null");
    return w.ch('{') && key(w, "function_id") && w.number(value.value.function_id) && w.ch(',') &&
           key(w, "local_id") && w.number(value.value.local_id) && w.ch('}');
}

bool systems(Writer& w, const Systems& value) noexcept
{
    bool comma = false;
    if (!w.ch('[')) return false;
    if ((value.mask & 1U) != 0U) {
        if (!w.text("\"ISDB-T\"")) return false;
        comma = true;
    }
    if ((value.mask & 2U) != 0U) {
        if (comma && !w.ch(',')) return false;
        if (!w.text("\"ISDB-S\"")) return false;
    }
    return w.ch(']');
}

bool receiver_descriptor(Writer& w, const ReceiverDescriptor& value) noexcept
{
    return w.ch('{') && key(w, "receiver") && w.number(value.id) && w.ch(',') &&
           key(w, "systems") && systems(w, value.systems) && w.ch(',') && key(w, "binding") &&
           binding(w, value.binding) && w.ch('}');
}

const char* receiver_state(ReceiverState value) noexcept
{
    switch (value) {
    case ReceiverState::free: return "free";
    case ReceiverState::leased: return "leased";
    case ReceiverState::tuned: return "tuned";
    case ReceiverState::streaming: return "streaming";
    case ReceiverState::error: return "error";
    case ReceiverState::absent: return "absent";
    }
    return "unknown";
}

const char* operation(Operation value) noexcept
{
    switch (value) {
    case Operation::idle: return "idle";
    case Operation::tuning: return "tuning";
    case Operation::stopping: return "stopping";
    case Operation::releasing: return "releasing";
    }
    return "unknown";
}

const char* verification(Verification value) noexcept
{
    switch (value) {
    case Verification::commands: return "commands";
    case Verification::lock: return "lock";
    case Verification::ts_sync: return "ts_sync";
    }
    return "unknown";
}

bool tune(Writer& w, const Optional<Tune>& value) noexcept
{
    if (!value.present) return w.text("null");
    const Tune& v = value.value;
    const char* system = v.system == System::isdb_t ? "ISDB-T" : "ISDB-S";
    const char* verify = verification(v.verification);
    if (!w.ch('{') || !key(w, "revision") || !w.quoted_u64(v.revision) || w.ch(',') == false ||
        !key(w, "system") || !w.string(system, length(system)) || w.ch(',') == false ||
        !key(w, "frequency_hz") || !w.number(v.frequency_hz) || w.ch(',') == false ||
        !key(w, "programmed_hz") ||
        (v.programmed_hz.present ? !w.number(v.programmed_hz.value) : !w.text("null")) ||
        !w.ch(',') || !key(w, "selection") || !w.ch('{') || !key(w, "kind")) {
        return false;
    }
    if (v.selection == Selection::automatic) {
        if (!w.text("\"auto\"}")) return false;
    } else {
        const char* kind = v.selection == Selection::tsid ? "tsid" : "slot";
        if (!w.string(kind, length(kind)) || !w.ch(',') || !key(w, "value") ||
            !w.number(v.selection_value) || !w.ch('}')) {
            return false;
        }
    }
    return w.ch(',') && key(w, "verification") && w.string(verify, length(verify)) && w.ch('}');
}

bool stream(Writer& w, const Optional<Stream>& value) noexcept
{
    if (!value.present) return w.text("null");
    const char* phase = "unknown";
    switch (value.value.phase) {
    case StreamPhase::armed: phase = "armed"; break;
    case StreamPhase::attaching: phase = "attaching"; break;
    case StreamPhase::active: phase = "active"; break;
    case StreamPhase::closing: phase = "closing"; break;
    }
    return w.ch('{') && key(w, "id") && w.quoted_u64(value.value.id) && w.ch(',') &&
           key(w, "tune_revision") && w.quoted_u64(value.value.tune_revision) && w.ch(',') &&
           key(w, "phase") && w.string(phase, length(phase)) && w.ch('}');
}

const char* end_reason(StreamEndReason value) noexcept
{
    switch (value) {
    case StreamEndReason::stopped: return "stopped";
    case StreamEndReason::data_closed: return "data_closed";
    case StreamEndReason::owner_closed: return "owner_closed";
    case StreamEndReason::attach_expired: return "attach_expired";
    case StreamEndReason::slow_consumer: return "slow_consumer";
    case StreamEndReason::device_lost: return "device_lost";
    case StreamEndReason::backend_error: return "backend_error";
    case StreamEndReason::shutdown: return "shutdown";
    }
    return "unknown";
}

bool counter(Writer& w, const Counter& value) noexcept
{
    const Measurement<std::uint64_t>& measurement = value.measurement;
    const char* reason = "not_sampled";
    switch (measurement.reason) {
    case UnknownReason::not_sampled: reason = "not_sampled"; break;
    case UnknownReason::stale: reason = "stale"; break;
    case UnknownReason::tune_changed: reason = "tune_changed"; break;
    case UnknownReason::device_lost: reason = "device_lost"; break;
    case UnknownReason::read_failed: reason = "read_failed"; break;
    }
    return w.ch('{') && key(w, "value") &&
           (measurement.state == MeasureState::valid
                ? (w.ch('"') && w.number(measurement.value) && w.ch('"'))
                : w.text("null")) &&
           w.ch(',') && key(w, "sampled_at_ns") &&
           (measurement.state == MeasureState::valid ? w.quoted_u64(measurement.sampled_at_ns)
                                                     : w.text("null")) &&
           w.ch(',') && key(w, "reason") &&
           (measurement.state == MeasureState::valid
                ? w.text("null")
                : w.string(reason, length(reason))) &&
           w.ch(',') && key(w, "overflow") &&
           (measurement.state == MeasureState::valid ? boolean(w, value.overflow)
                                                     : w.text("null")) &&
           w.ch('}');
}

const char* counter_name(std::uint8_t id) noexcept
{
    switch (id) {
    case kCounterIdCapturedPackets: return "captured_packets";
    case kCounterIdUsbErrors: return "usb_errors";
    case kCounterIdFramingErrors: return "framing_errors";
    case kCounterIdUpstreamLostPackets: return "upstream_lost_packets";
    case kCounterIdTransmittedPackets: return "transmitted_packets";
    default: return "queue_dropped_packets";
    }
}

template <typename Counters>
bool counters(Writer& w, const Counters& value) noexcept
{
    if (!w.ch('{')) return false;
    for (std::size_t i = 0U; i < value.entries.count; ++i) {
        const char* name = counter_name(value.entries.items[i].id);
        if ((i != 0U && !w.ch(',')) || !key(w, name) ||
            !counter(w, value.entries.items[i].counter)) {
            return false;
        }
    }
    return w.ch('}');
}

bool metric(Writer& w, const MetricEntry& value) noexcept
{
    const bool lock = value.id == kMetricIdLock;
    const MeasureState state = lock ? value.boolean.state : value.integer.state;
    if (state == MeasureState::unsupported) return w.text("null");
    const char* name = value.id == kMetricIdLock ? "lock"
                       : value.id == kMetricIdCnrMdb ? "cnr_mdb"
                                                     : "signal_mdbm";
    const UnknownReason unknown_reason = lock ? value.boolean.reason : value.integer.reason;
    const char* reason = "not_sampled";
    switch (unknown_reason) {
    case UnknownReason::not_sampled: reason = "not_sampled"; break;
    case UnknownReason::stale: reason = "stale"; break;
    case UnknownReason::tune_changed: reason = "tune_changed"; break;
    case UnknownReason::device_lost: reason = "device_lost"; break;
    case UnknownReason::read_failed: reason = "read_failed"; break;
    }
    if (!key(w, name) || !w.ch('{') || !key(w, "value")) return false;
    if (state == MeasureState::unknown) {
        return w.text("null,") && key(w, "sampled_at_ns") && w.text("null,") && key(w, "reason") &&
               w.string(reason, length(reason)) && w.ch('}');
    }
    const std::uint64_t sampled = lock ? value.boolean.sampled_at_ns : value.integer.sampled_at_ns;
    const bool written = lock ? boolean(w, value.boolean.value)
                              : w.signed_number(value.integer.value);
    return written && w.ch(',') && key(w, "sampled_at_ns") && w.quoted_u64(sampled) && w.ch(',') &&
           key(w, "reason") && w.text("null}");
}

bool measurements(Writer& w, const Measurements& value) noexcept
{
    if (!w.ch('{')) return false;
    bool comma = false;
    for (std::size_t i = 0U; i < value.entries.count; ++i) {
        const MetricEntry& entry = value.entries.items[i];
        if ((entry.id == kMetricIdLock && entry.boolean.state == MeasureState::unsupported) ||
            (entry.id != kMetricIdLock && entry.integer.state == MeasureState::unsupported)) {
            continue;
        }
        if (comma && !w.ch(',')) return false;
        comma = true;
        if (!metric(w, entry)) return false;
    }
    return w.ch('}');
}

bool final_stream(Writer& w, const Optional<StreamFinal>& value) noexcept
{
    if (!value.present) return w.text("null");
    const char* reason = end_reason(value.value.reason);
    return w.ch('{') && key(w, "id") && w.quoted_u64(value.value.id) && w.ch(',') &&
           key(w, "tune_revision") && w.quoted_u64(value.value.tune_revision) && w.ch(',') &&
           key(w, "reason") && w.string(reason, length(reason)) && w.ch(',') && key(w, "error") &&
           error(w, value.value.error) && w.ch(',') && key(w, "statistics") &&
           counters(w, value.value.statistics) && w.ch('}');
}

bool receiver_status(Writer& w, const ReceiverStatus& value) noexcept
{
    const char* state = receiver_state(value.state);
    const char* op = operation(value.operation);
    return w.ch('{') && key(w, "descriptor") && receiver_descriptor(w, value.descriptor) &&
           w.ch(',') && key(w, "present") && boolean(w, value.present) && w.ch(',') &&
           key(w, "state") && w.string(state, length(state)) && w.ch(',') && key(w, "operation") &&
           w.string(op, length(op)) && w.ch(',') && key(w, "leased") && boolean(w, value.leased) &&
           w.ch(',') && key(w, "tune_revision") && w.quoted_u64(value.tune_revision) && w.ch(',') &&
           key(w, "current_tune") && tune(w, value.current_tune) && w.ch(',') &&
           key(w, "last_successful_tune") && tune(w, value.last_successful_tune) && w.ch(',') &&
           key(w, "stream") && stream(w, value.stream) && w.ch(',') && key(w, "last_stream") &&
           final_stream(w, value.last_stream) && w.ch(',') && key(w, "measurements") &&
           measurements(w, value.measurements) && w.ch(',') && key(w, "statistics") && w.ch('{') &&
           key(w, "receiver") && counters(w, value.receiver_statistics) && w.ch(',') &&
           key(w, "stream") &&
           (value.stream_statistics.present ? counters(w, value.stream_statistics.value)
                                            : w.text("null")) &&
           w.text("}") && w.ch(',') && key(w, "last_error") && error(w, value.last_error) &&
           w.ch('}');
}

const char* card_state(CardState value) noexcept
{
    switch (value) {
    case CardState::unknown: return "unknown";
    case CardState::absent: return "absent";
    case CardState::present: return "present";
    case CardState::unavailable: return "unavailable";
    }
    return "unknown";
}

const char* card_reason(std::uint8_t reason) noexcept
{
    switch (static_cast<UnknownReason>(reason)) {
    case UnknownReason::not_sampled: return "not_sampled";
    case UnknownReason::stale: return "stale";
    case UnknownReason::tune_changed: return "tune_changed";
    case UnknownReason::device_lost: return "device_lost";
    case UnknownReason::read_failed: return "read_failed";
    }
    return "not_sampled";
}

const char* card_operation(CardOperation value) noexcept
{
    switch (value) {
    case CardOperation::idle: return "idle";
    case CardOperation::initializing: return "initializing";
    case CardOperation::transmitting: return "transmitting";
    case CardOperation::resetting: return "resetting";
    case CardOperation::stopping: return "stopping";
    }
    return "unknown";
}

bool reader_descriptor(Writer& w, const ReaderDescriptor& value) noexcept
{
    return w.ch('{') && key(w, "reader") && w.number(value.id) && w.ch(',') && key(w, "binding") &&
           binding(w, value.binding) && w.ch('}');
}

bool reader_status(Writer& w, const ReaderStatus& value) noexcept
{
    const char* state = card_state(value.card.state);
    const char* operation_value = card_operation(value.operation);
    const bool sampled = value.card.sampled_at_ns.present;
    const char* reason = card_reason(value.card.reason);
    return w.ch('{') && key(w, "descriptor") && reader_descriptor(w, value.descriptor) &&
           w.ch(',') && key(w, "present") && boolean(w, value.present) && w.ch(',') &&
           key(w, "card") && w.ch('{') && key(w, "state") && w.string(state, length(state)) &&
           w.ch(',') && key(w, "sampled_at_ns") &&
           (sampled ? w.quoted_u64(value.card.sampled_at_ns.value) : w.text("null")) && w.ch(',') &&
           key(w, "reason") && (sampled ? w.text("null") : w.string(reason, length(reason))) &&
           w.text("}") && w.ch(',') && key(w, "operation") &&
           w.string(operation_value, length(operation_value)) && w.ch(',') && key(w, "handles_open") &&
           w.number(value.handles_open) && w.ch(',') && key(w, "transaction_active") &&
           boolean(w, value.transaction_active) && w.ch(',') && key(w, "last_error") &&
           error(w, value.last_error) && w.ch('}');
}

bool emit(const StatusResponse& value, Writer& w) noexcept
{
    if (!w.ch('{') || !key(w, "schema") ||
        !w.text("{\"name\":\"userland.status\",\"major\":1,\"minor\":0}") ||
        !w.ch(',') || !key(w, "producer") || !producer(w, value.producer) || w.ch(',') == false ||
        !key(w, "context") || !context(w, value.context) || w.ch(',') == false ||
        !key(w, "snapshot") || !w.ch('{') || !key(w, "sequence") ||
        !w.quoted_u64(value.snapshot.sequence) || !w.ch(',') || !key(w, "observed_at_ns") ||
        !w.quoted_u64(value.snapshot.observed_at_ns) || !w.ch('}') || w.ch(',') == false ||
        !key(w, "scope") || !scope(w, value.scope) || w.ch(',') == false || !key(w, "enclosure") ||
        !enclosure(w, value) || w.ch(',') == false || !key(w, "topology") ||
        !topology(w, value.topology)) {
        return false;
    }
    if (value.functions.present) {
        if (!w.ch(',') || !key(w, "usb_functions") || !w.ch('[')) return false;
        for (std::size_t i = 0U; i < value.functions.value.count; ++i) {
            if ((i != 0U && !w.ch(',')) || !usb_function(w, value.functions.value.items[i])) return false;
        }
        if (!w.ch(']')) return false;
    }
    if (value.receivers.present) {
        if (!w.ch(',') || !key(w, "receivers") || !w.ch('[')) return false;
        for (std::size_t i = 0U; i < value.receivers.value.count; ++i) {
            if ((i != 0U && !w.ch(',')) ||
                !receiver_status(w, value.receivers.value.items[i])) return false;
        }
        if (!w.ch(']')) return false;
    }
    if (value.readers.present) {
        if (!w.ch(',') || !key(w, "readers") || !w.ch('[')) return false;
        for (std::size_t i = 0U; i < value.readers.value.count; ++i) {
            if ((i != 0U && !w.ch(',')) || !reader_status(w, value.readers.value.items[i])) return false;
        }
        if (!w.ch(']')) return false;
    }
    return w.text("}\n");
}

}  // namespace

Result<std::size_t> status_json_size(const StatusResponse& value,
                                     const StatusContext& context_value) noexcept
{
    const Error valid = validate_status_response(value, context_value);
    if (valid != Error::OK) return Result<std::size_t>::failure(valid);
    Writer count({nullptr, 0U});
    if (!emit(value, count)) return Result<std::size_t>::failure(Error::RESOURCE_LIMIT);
    return Result<std::size_t>::success(count.size());
}

Result<std::size_t> encode_status_json(const StatusResponse& value,
                                       const StatusContext& context_value,
                                       MutableByteView output) noexcept
{
    const Error valid = validate_status_response(value, context_value);
    if (valid != Error::OK) return Result<std::size_t>::failure(valid);
    if (output.data == nullptr && output.size != 0U) {
        return Result<std::size_t>::failure(Error::INVALID_ARGUMENT);
    }
    Writer count({nullptr, 0U});
    if (!emit(value, count)) return Result<std::size_t>::failure(Error::RESOURCE_LIMIT);
    if (output.size < count.size()) return Result<std::size_t>::failure(Error::BUFFER_TOO_SMALL);
    Writer write(output);
    if (!emit(value, write) || write.size() != count.size()) {
        return Result<std::size_t>::failure(Error::RESOURCE_LIMIT);
    }
    return Result<std::size_t>::success(write.size());
}

}  // namespace dtv::userland::wire
