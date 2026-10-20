// SPDX-License-Identifier: MIT
#include <dtv/userland/json.h>

#include <dtv/userland/error.h>
#include <dtv/userland/identity.h>

#include <limits>
#include <string_view>

namespace dtv::userland::wire {
namespace {

class Writer final {
public:
    explicit Writer(MutableByteView output) noexcept : output_(output) {}

    bool ch(char value) noexcept
    {
        if (size_ == std::numeric_limits<std::size_t>::max()) return false;
        if (output_.data != nullptr && size_ < output_.size) output_.data[size_] = static_cast<std::uint8_t>(value);
        ++size_;
        return true;
    }

    bool text(const char* value) noexcept
    {
        while (*value != '\0') if (!ch(*value++)) return false;
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
            } else if (!ch(static_cast<char>(c))) return false;
        }
        return ch('"');
    }

    bool number(std::uint64_t value) noexcept
    {
        char digits[20];
        std::size_t count = 0U;
        do { digits[count++] = static_cast<char>('0' + value % 10U); value /= 10U; } while (value != 0U);
        while (count != 0U) if (!ch(digits[--count])) return false;
        return true;
    }

    bool signed_number(std::int32_t value) noexcept
    {
        if (value < 0) {
            if (!ch('-')) return false;
            return number(static_cast<std::uint64_t>(-(static_cast<std::int64_t>(value))));
        }
        return number(static_cast<std::uint64_t>(value));
    }

    bool quoted_number(std::uint64_t value) noexcept { return ch('"') && number(value) && ch('"'); }
    std::size_t size() const noexcept { return size_; }

private:
    MutableByteView output_;
    std::size_t size_ = 0U;
};

template <std::size_t N>
bool str(Writer& w, const WString<N>& value) noexcept { return w.string(value.bytes.data(), value.length); }

std::size_t length(const char* value) noexcept { std::size_t size=0U; while(value[size]!='\0') ++size; return size; }
bool key(Writer& w, const char* name) noexcept { return w.string(name, length(name)) && w.ch(':'); }
bool boolean(Writer& w, bool value) noexcept { return w.text(value ? "true" : "false"); }
bool nullable_u64(Writer& w, const Optional<std::uint64_t>& value) noexcept
{ return value.present ? w.quoted_number(value.value) : w.text("null"); }

bool producer(Writer& w, const Producer& v) noexcept
{
    return w.ch('{') && key(w,"product") && str(w,v.product) && w.ch(',') && key(w,"version") && str(w,v.version) &&
        w.ch(',') && key(w,"core_version") && (v.core_version.present ? str(w,v.core_version.value) : w.text("null")) && w.ch('}');
}
bool model(Writer& w, const Optional<Model>& v) noexcept
{
    if (!v.present) return w.text("null");
    return w.ch('{') && key(w,"id") && str(w,v.value.id) && w.ch(',') && key(w,"name") && str(w,v.value.name) && w.ch('}');
}
const char* identity_state(IdentitySerialState v) noexcept { return v==IdentitySerialState::present?"present":v==IdentitySerialState::absent?"absent":"unknown"; }
const char* uniqueness(Uniqueness v) noexcept { return v==Uniqueness::not_applicable?"not_applicable":v==Uniqueness::unique_in_scan?"unique_in_scan":v==Uniqueness::duplicate?"duplicate":"unknown"; }
bool identity(Writer& w,const Identity& v) noexcept
{
    return w.ch('{')&&key(w,"serial")&&(v.serial.present?str(w,v.serial.value):w.text("null"))&&w.ch(',')&&key(w,"serial_state")&&w.string(identity_state(v.serial_state),length(identity_state(v.serial_state)))&&w.ch(',')&&key(w,"uniqueness")&&w.string(uniqueness(v.uniqueness),length(uniqueness(v.uniqueness)))&&w.ch('}');
}
bool limits(Writer& w,const Limits& v) noexcept
{ return w.ch('{')&&key(w,"max_concurrent_streams")&&(v.max_concurrent_streams.present?w.number(v.max_concurrent_streams.value):w.text("null"))&&w.ch('}'); }
const char* topology_state(TopologyState v) noexcept { return v==TopologyState::complete?"complete":v==TopologyState::partial?"partial":"unknown"; }
bool topology(Writer& w,const Topology& v) noexcept
{ const char* s=topology_state(v.state); return w.ch('{')&&key(w,"state")&&w.string(s,length(s))&&w.ch(',')&&key(w,"expected_function_count")&&(v.expected_function_count.present?w.number(v.expected_function_count.value):w.text("null"))&&w.ch('}'); }
bool enclosure(Writer& w,const Optional<Enclosure>& v,
               const Optional<bool>& lnb_15v_enabled) noexcept
{
    if(!v.present)return w.text("null");
    return w.ch('{')&&key(w,"id")&&w.quoted_number(v.value.id)&&w.ch(',')&&key(w,"model")&&model(w,v.value.model)&&w.ch(',')&&key(w,"identity")&&identity(w,v.value.identity)&&w.ch(',')&&key(w,"limits")&&limits(w,v.value.limits)&&w.ch(',')&&key(w,"lnb_15v_enabled")&&(lnb_15v_enabled.present?boolean(w,lnb_15v_enabled.value):w.text("null"))&&w.ch('}');
}
const char* mode(Mode v) noexcept{return v==Mode::cold?"cold":v==Mode::runtime?"runtime":"unknown";}
const char* serial_state(SerialState v) noexcept{return v==SerialState::present?"present":v==SerialState::absent?"absent":v==SerialState::unreadable?"unreadable":v==SerialState::invalid?"invalid":"not_queried";}
const char* identification_reason(IdentificationReason v) noexcept{return v==IdentificationReason::cold_model?"cold_model":v==IdentificationReason::descriptor_unreadable?"descriptor_unreadable":v==IdentificationReason::model_unconfirmed?"model_unconfirmed":"none";}
bool error(Writer& w,const Optional<ErrorRecord>& v) noexcept
{
    if(!v.present)return w.text("null");
    const char* symbol=dtv::userland::error_symbol(v.value.code);
    if(!w.ch('{')||!key(w,"code")||!w.string(symbol?symbol:"UNKNOWN",symbol?length(symbol):7U))return false;
    if(!symbol && (!w.ch(',')||!key(w,"numeric_code")||!w.number(v.value.code)))return false;
    return w.ch(',')&&key(w,"operation")&&str(w,v.value.operation)&&w.ch(',')&&key(w,"detail")&&str(w,v.value.detail)&&w.ch('}');
}
// Shared observation fields used by STATUS and discovery. The caller closes
// the outer object with its own snapshot-specific fields.
bool function_observation(Writer& w,const UsbFunction& v) noexcept
{
    const char* m=mode(v.mode); const char* ss=serial_state(v.serial_state); const char* ir=identification_reason(v.identification_reason);
    static constexpr char h[]="0123456789abcdef";
    if(!w.ch('{')||!key(w,"id")||!w.quoted_number(v.id)||!w.ch(',')||!key(w,"vid"))return false;
    if(!w.ch('"')||!w.ch(h[(v.vid>>12U)&15U])||!w.ch(h[(v.vid>>8U)&15U])||!w.ch(h[(v.vid>>4U)&15U])||!w.ch(h[v.vid&15U])||!w.ch('"')||!w.ch(',')||!key(w,"pid"))return false;
    if(!w.ch('"')||!w.ch(h[(v.pid>>12U)&15U])||!w.ch(h[(v.pid>>8U)&15U])||!w.ch(h[(v.pid>>4U)&15U])||!w.ch(h[v.pid&15U])||!w.ch('"')||!w.ch(',')||!key(w,"mode")||!w.string(m,length(m))||!w.ch(',')||!key(w,"location")||!w.ch('{')||!key(w,"usb_path")||(v.location.usb_path.present?!str(w,v.location.usb_path.value):!w.text("null"))||!w.ch(',')||!key(w,"bus")||(v.location.bus.present?!w.number(v.location.bus.value):!w.text("null"))||!w.ch(',')||!key(w,"address")||(v.location.address.present?!w.number(v.location.address.value):!w.text("null"))||!w.ch('}')||!w.ch(',')||!key(w,"serial")||!w.ch('{')||!key(w,"state")||!w.string(ss,length(ss))||!w.ch(',')||!key(w,"value")||(v.serial.present?!str(w,v.serial.value):!w.text("null"))||!w.ch('}')||!w.ch(',')||!key(w,"identification")||!w.ch('{')||!key(w,"model")||!model(w,v.model)||!w.ch(',')||!key(w,"reason")||(v.identification_reason==IdentificationReason::none?!w.text("null"):!w.string(ir,length(ir)))||!w.ch('}'))return false;
    return true;
}
bool function(Writer& w,const UsbFunction& v) noexcept
{
    return function_observation(w,v)&&w.ch(',')&&key(w,"present")&&boolean(w,v.present)&&w.ch(',')&&key(w,"last_error")&&error(w,v.last_error)&&w.ch('}');
}
bool binding(Writer& w,const Optional<Binding>& v) noexcept
{return !v.present?w.text("null"):(w.ch('{')&&key(w,"function_id")&&w.quoted_number(v.value.function_id)&&w.ch(',')&&key(w,"local_id")&&w.quoted_number(v.value.local_id)&&w.ch('}'));}
bool capabilities(Writer& w,const Capabilities& v) noexcept
{
    if(!w.ch('{')||!key(w,"ts")||!w.ch('{')||!key(w,"scope")||!w.text(v.ts_scope==TsScope::pid_subset?"\"pid_subset\"":"\"multiplex\"")||!w.ch(',')||!key(w,"pid_filter"))return false;
    if(v.max_pids.present){if(!w.ch('{')||!key(w,"max_pids")||!w.number(v.max_pids.value)||!w.ch('}'))return false;}else if(!w.text("null"))return false;
    if(!w.ch('}')||!w.ch(',')||!key(w,"controls")||!w.ch('{'))return false;
    bool comma=false;
    if((v.controls&kControlBitLna)!=0U){if(!key(w,"lna")||!w.ch('{')||!key(w,"values")||!w.text("[false,true]")||!w.ch(',')||!key(w,"default_by_system")||!w.ch('{'))return false;for(std::size_t i=0;i<v.lna.entries.count;++i){const auto& e=v.lna.entries.items[i];const char* name=e.system==System::isdb_s?"isdb-s":"isdb-t";if(i&& !w.ch(','))return false;if(!key(w,name)||(e.default_value.present?!boolean(w,e.default_value.value):!w.text("null")))return false;}if(!w.ch('}')||!w.ch('}'))return false;comma=true;}
    if((v.controls&kControlBitLnb)!=0U){if(comma&&!w.ch(','))return false;if(!key(w,"lnb_voltage")||!w.ch('{')||!key(w,"values")||!w.ch('['))return false;for(std::size_t i=0;i<v.lnb.values.count;++i){if(i&&!w.ch(','))return false;if(!w.number(v.lnb.values.items[i]))return false;}if(!w.text("]}") )return false;}
    if(!w.ch('}')||!w.ch(',')||!key(w,"measurements")||!w.ch('{'))return false;
    bool first=true;const char* names[]={"lock","cnr_mdb","signal_mdbm"};const char* units[]={"boolean","milli_dB","milli_dBm"};
    for(std::size_t i=0;i<3U;++i)if((v.measurement_mask&(1U<<i))!=0U){if(!first&&!w.ch(','))return false;first=false;if(!key(w,names[i])||!w.ch('{')||!key(w,"unit")||!w.string(units[i],length(units[i]))||!w.ch('}'))return false;}
    return w.ch('}')&&w.ch('}');
}
bool receiver_descriptor(Writer& w,const ReceiverDescriptor& v) noexcept
{
    if(!w.ch('{')||!key(w,"id")||!w.quoted_number(v.id)||!w.ch(',')||!key(w,"systems")||!w.ch('['))return false;
    bool comma=false;if((v.systems.mask&1U)!=0U){if(!w.text("\"isdb-t\""))return false;comma=true;}if((v.systems.mask&2U)!=0U){if(comma&&!w.ch(','))return false;if(!w.text("\"isdb-s\""))return false;}
    return w.ch(']')&&w.ch(',')&&key(w,"binding")&&binding(w,v.binding)&&w.ch(',')&&key(w,"capabilities")&&capabilities(w,v.capabilities)&&w.ch('}');
}
bool reader_descriptor(Writer& w,const ReaderDescriptor& v) noexcept
{return w.ch('{')&&key(w,"id")&&w.quoted_number(v.id)&&w.ch(',')&&key(w,"binding")&&binding(w,v.binding)&&w.ch('}');}
bool metric(Writer& w,const MetricEntry& v) noexcept
{
    const bool lock=v.id==kMetricIdLock;const auto state=lock?v.boolean.state:v.integer.state;
    if(state==MeasureState::unsupported)return w.text("null");
    if(!w.ch('{')||!key(w,"value"))return false;
    if(state==MeasureState::unknown){if(!w.text("null")||!w.ch(',')||!key(w,"sampled_at_ns")||!w.text("null")||!w.ch(',')||!key(w,"reason"))return false;const auto reason=lock?v.boolean.reason:v.integer.reason;const char* s=reason==UnknownReason::stale?"stale":reason==UnknownReason::tune_changed?"tune_changed":reason==UnknownReason::device_lost?"device_lost":reason==UnknownReason::read_failed?"read_failed":"not_sampled";return w.string(s,length(s))&&w.ch('}');}
    if(!(lock?boolean(w,v.boolean.value):w.signed_number(v.integer.value))||!w.ch(',')||!key(w,"sampled_at_ns")||!w.quoted_number(lock?v.boolean.sampled_at_ns:v.integer.sampled_at_ns)||!w.ch(',')||!key(w,"reason")||!w.text("null"))return false;
    return w.ch('}');
}
bool measurements(Writer& w,const Measurements& v) noexcept
{
    if(!w.ch('{'))return false;
    bool comma=false;const char* names[]={"lock","cnr_mdb","signal_mdbm"};
    for(std::size_t i=0;i<v.entries.count;++i){const auto& e=v.entries.items[i];if(e.id==0U||e.id>3U)continue;if(comma&&!w.ch(','))return false;comma=true;if(!key(w,names[e.id-1U])||!metric(w,e))return false;}return w.ch('}');
}
const char* counter_name(std::uint8_t id) noexcept {switch(id){case 1:return "captured_packets";case 2:return "usb_errors";case 3:return "framing_errors";case 4:return "upstream_lost_packets";case 5:return "transmitted_packets";default:return "queue_dropped_packets";}}
bool counter(Writer& w,const Counter& v) noexcept
{
    const auto& m=v.measurement;if(!w.ch('{')||!key(w,"value")||!nullable_u64(w,Optional<std::uint64_t>{m.state==MeasureState::valid,m.value})||!w.ch(',')||!key(w,"sampled_at_ns")||!(m.state==MeasureState::valid?w.quoted_number(m.sampled_at_ns):w.text("null"))||!w.ch(',')||!key(w,"reason"))return false;
    if(m.state==MeasureState::valid){if(!w.text("null"))return false;}else{const char* s=m.reason==UnknownReason::stale?"stale":m.reason==UnknownReason::tune_changed?"tune_changed":m.reason==UnknownReason::device_lost?"device_lost":m.reason==UnknownReason::read_failed?"read_failed":"not_sampled";if(!w.string(s,length(s)))return false;}
    return w.ch(',')&&key(w,"overflow")&&(m.state==MeasureState::valid?boolean(w,v.overflow):w.text("null"))&&w.ch('}');
}
template <typename A> bool counters(Writer& w,const A& v) noexcept
{if(!w.ch('{'))return false;for(std::size_t i=0;i<v.entries.count;++i){if(i&&!w.ch(','))return false;if(!key(w,counter_name(v.entries.items[i].id))||!counter(w,v.entries.items[i].counter))return false;}return w.ch('}');}
bool selection(Writer& w,const Tune& v) noexcept
{if(!w.ch('{')||!key(w,"kind"))return false;const char* k=v.selection==Selection::tsid?"tsid":v.selection==Selection::slot?"slot":"auto";if(!w.string(k,length(k)))return false;if(v.selection!=Selection::automatic)return w.ch(',')&&key(w,"value")&&w.number(v.selection_value)&&w.ch('}');return w.ch('}');}
bool tune(Writer& w,const Optional<Tune>& v) noexcept
{if(!v.present)return w.text("null");const auto& t=v.value;const char* sys=t.system==System::isdb_s?"isdb-s":"isdb-t";const char* ver=t.verification==Verification::lock?"lock":t.verification==Verification::ts_sync?"ts_sync":"commands";return w.ch('{')&&key(w,"revision")&&w.quoted_number(t.revision)&&w.ch(',')&&key(w,"system")&&w.string(sys,length(sys))&&w.ch(',')&&key(w,"frequency_hz")&&w.number(t.frequency_hz)&&w.ch(',')&&key(w,"programmed_hz")&&(t.programmed_hz.present?w.number(t.programmed_hz.value):w.text("null"))&&w.ch(',')&&key(w,"selection")&&selection(w,t)&&w.ch(',')&&key(w,"verification")&&w.string(ver,length(ver))&&w.ch('}');}
bool stream(Writer& w,const Optional<Stream>& v) noexcept
{if(!v.present)return w.text("null");const char* p=v.value.phase==StreamPhase::attaching?"attaching":v.value.phase==StreamPhase::active?"active":v.value.phase==StreamPhase::closing?"closing":"armed";return w.ch('{')&&key(w,"id")&&w.quoted_number(v.value.id)&&w.ch(',')&&key(w,"tune_revision")&&w.quoted_number(v.value.tune_revision)&&w.ch(',')&&key(w,"phase")&&w.string(p,length(p))&&w.ch('}');}
const char* end_reason(StreamEndReason v) noexcept {switch(v){case StreamEndReason::data_closed:return "data_closed";case StreamEndReason::owner_closed:return "owner_closed";case StreamEndReason::attach_expired:return "attach_expired";case StreamEndReason::slow_consumer:return "slow_consumer";case StreamEndReason::device_lost:return "device_lost";case StreamEndReason::backend_error:return "backend_error";case StreamEndReason::shutdown:return "shutdown";default:return "stopped";}}
bool final_stream(Writer& w,const Optional<StreamFinal>& v) noexcept
{if(!v.present)return w.text("null");const auto& f=v.value;return w.ch('{')&&key(w,"id")&&w.quoted_number(f.id)&&w.ch(',')&&key(w,"tune_revision")&&w.quoted_number(f.tune_revision)&&w.ch(',')&&key(w,"reason")&&w.string(end_reason(f.reason),length(end_reason(f.reason)))&&w.ch(',')&&key(w,"error")&&error(w,f.error)&&w.ch(',')&&key(w,"statistics")&&counters(w,f.statistics)&&w.ch('}');}
bool receiver_status(Writer& w,const ReceiverStatus& v) noexcept
{
    static const char* states[]={"free","leased","tuned","streaming","error","absent"};static const char* ops[]={"idle","tuning","stopping","releasing"};
    return w.ch('{')&&key(w,"descriptor")&&receiver_descriptor(w,v.descriptor)&&w.ch(',')&&key(w,"present")&&boolean(w,v.present)&&w.ch(',')&&key(w,"state")&&w.string(states[static_cast<unsigned>(v.state)],length(states[static_cast<unsigned>(v.state)]))&&w.ch(',')&&key(w,"operation")&&w.string(ops[static_cast<unsigned>(v.operation)],length(ops[static_cast<unsigned>(v.operation)]))&&w.ch(',')&&key(w,"leased")&&boolean(w,v.leased)&&w.ch(',')&&key(w,"tune_revision")&&w.quoted_number(v.tune_revision)&&w.ch(',')&&key(w,"current_tune")&&tune(w,v.current_tune)&&w.ch(',')&&key(w,"last_successful_tune")&&tune(w,v.last_successful_tune)&&w.ch(',')&&key(w,"stream")&&stream(w,v.stream)&&w.ch(',')&&key(w,"last_stream")&&final_stream(w,v.last_stream)&&w.ch(',')&&key(w,"measurements")&&measurements(w,v.measurements)&&w.ch(',')&&key(w,"statistics")&&w.ch('{')&&key(w,"receiver")&&counters(w,v.receiver_statistics)&&w.ch(',')&&key(w,"stream")&&(v.stream_statistics.present?counters(w,v.stream_statistics.value):w.text("null"))&&w.ch('}')&&w.ch(',')&&key(w,"last_error")&&error(w,v.last_error)&&w.ch('}');
}
const char* card_state(CardState v) noexcept{return v==CardState::absent?"absent":v==CardState::present?"present":v==CardState::unavailable?"unavailable":"unknown";}
const char* card_reason(std::uint8_t v) noexcept{return v==2U?"stale":v==4U?"device_lost":v==5U?"read_failed":"not_sampled";}
bool reader_status(Writer& w,const ReaderStatus& v) noexcept
{static const char* ops[]={"idle","initializing","transmitting","resetting","stopping"};const char* cs=card_state(v.card.state);const bool sampled=v.card.sampled_at_ns.present;return w.ch('{')&&key(w,"descriptor")&&reader_descriptor(w,v.descriptor)&&w.ch(',')&&key(w,"present")&&boolean(w,v.present)&&w.ch(',')&&key(w,"card")&&w.ch('{')&&key(w,"state")&&w.string(cs,length(cs))&&w.ch(',')&&key(w,"sampled_at_ns")&&nullable_u64(w,v.card.sampled_at_ns)&&w.ch(',')&&key(w,"reason")&&(sampled?w.text("null"):w.string(card_reason(v.card.reason),length(card_reason(v.card.reason))))&&w.ch('}')&&w.ch(',')&&key(w,"operation")&&w.string(ops[static_cast<unsigned>(v.operation)],length(ops[static_cast<unsigned>(v.operation)]))&&w.ch(',')&&key(w,"handles_open")&&w.number(v.handles_open)&&w.ch(',')&&key(w,"transaction_active")&&boolean(w,v.transaction_active)&&w.ch(',')&&key(w,"last_error")&&error(w,v.last_error)&&w.ch('}');}

bool emit(const StatusResponse& v,Writer& w) noexcept
{
    static constexpr char hex[]="0123456789abcdef";const char* states[]={"starting","ready","stopping","error","disconnected"};
    if(!w.ch('{')||!key(w,"schema")||!w.text("{\"name\":\"userland.status\",\"major\":1,\"minor\":0}")||!w.ch(',')||!key(w,"producer")||!producer(w,v.producer)||!w.ch(',')||!key(w,"context")||!w.ch('{')||!key(w,"epoch")||!w.ch('"'))return false;
    for(auto b:v.context.epoch){if(!w.ch(hex[(b>>4U)&15U])||!w.ch(hex[b&15U]))return false;}
    if(!w.ch('"')||!w.ch(',')||!key(w,"instance")||(v.context.instance.present?!str(w,v.context.instance.value):!w.text("null"))||!w.ch(',')||!key(w,"state")||!w.string(states[static_cast<unsigned>(v.context.state)],length(states[static_cast<unsigned>(v.context.state)]))||!w.ch(',')||!key(w,"last_error")||!error(w,v.context.last_error)||!w.ch('}')||!w.ch(',')||!key(w,"snapshot")||!w.ch('{')||!key(w,"sequence")||!w.quoted_number(v.snapshot.sequence)||!w.ch(',')||!key(w,"observed_at_ns")||!w.quoted_number(v.snapshot.observed_at_ns)||!w.ch('}')||!w.ch(',')||!key(w,"scope")||!w.ch('{')||!key(w,"kind"))return false;
    const char* scope=v.scope.kind==ScopeKind::receiver?"receiver":v.scope.kind==ScopeKind::reader?"reader":v.scope.kind==ScopeKind::stream?"stream":"enclosure";
    if(!w.string(scope,length(scope))||!w.ch(',')||!key(w,"id")||(v.scope.kind==ScopeKind::enclosure?!w.text("null"):!w.quoted_number(v.scope.id))||!w.ch('}')||!w.ch(',')||!key(w,"enclosure")||!enclosure(w,v.enclosure,v.lnb_15v_enabled)||!w.ch(',')||!key(w,"topology")||!topology(w,v.topology))return false;
    if(v.functions.present){if(!w.ch(',')||!key(w,"usb_functions")||!w.ch('['))return false;std::array<std::uint8_t,kMaxFunctions> order{};for(std::size_t i=0;i<v.functions.value.count;++i)order[i]=static_cast<std::uint8_t>(i);for(std::size_t i=0;i<v.functions.value.count;++i)for(std::size_t j=i+1;j<v.functions.value.count;++j)if(v.functions.value.items[order[j]].id<v.functions.value.items[order[i]].id){auto t=order[i];order[i]=order[j];order[j]=t;}for(std::size_t i=0;i<v.functions.value.count;++i){if(i&&!w.ch(','))return false;if(!function(w,v.functions.value.items[order[i]]))return false;}if(!w.ch(']'))return false;}
    if(v.receivers.present){if(!w.ch(',')||!key(w,"receivers")||!w.ch('['))return false;std::array<std::uint8_t,kMaxPublicReceivers> order{};for(std::size_t i=0;i<v.receivers.value.count;++i)order[i]=static_cast<std::uint8_t>(i);for(std::size_t i=0;i<v.receivers.value.count;++i)for(std::size_t j=i+1;j<v.receivers.value.count;++j)if(v.receivers.value.items[order[j]].descriptor.id<v.receivers.value.items[order[i]].descriptor.id){auto t=order[i];order[i]=order[j];order[j]=t;}for(std::size_t i=0;i<v.receivers.value.count;++i){if(i&&!w.ch(','))return false;if(!receiver_status(w,v.receivers.value.items[order[i]]))return false;}if(!w.ch(']'))return false;}
    if(v.readers.present){if(!w.ch(',')||!key(w,"readers")||!w.ch('['))return false;std::array<std::uint8_t,kMaxReaders> order{};for(std::size_t i=0;i<v.readers.value.count;++i)order[i]=static_cast<std::uint8_t>(i);for(std::size_t i=0;i<v.readers.value.count;++i)for(std::size_t j=i+1;j<v.readers.value.count;++j)if(v.readers.value.items[order[j]].descriptor.id<v.readers.value.items[order[i]].descriptor.id){auto t=order[i];order[i]=order[j];order[j]=t;}for(std::size_t i=0;i<v.readers.value.count;++i){if(i&&!w.ch(','))return false;if(!reader_status(w,v.readers.value.items[order[i]]))return false;}if(!w.ch(']'))return false;}
    return w.text("}\n");
}


template<std::size_t N>
bool valid_text(const WString<N>& v,bool required=true) noexcept
{
    if(v.length>N||(required&&v.length==0U))return false;
    for(std::size_t i=0U;i<v.length;++i)if(v.bytes[i]=='\0')return false;
    return detail::strict_utf8(reinterpret_cast<const std::uint8_t*>(v.bytes.data()),v.length);
}
template<std::size_t N>
bool valid_optional_text(const Optional<WString<N>>& v) noexcept
{ return !v.present||valid_text(v.value); }

bool valid_model(const Optional<Model>& v) noexcept
{
    return !v.present||(valid_text(v.value.id)&&valid_text(v.value.name));
}

bool valid_candidate(const DiscoverySelectorCandidate& v) noexcept
{
    if(!valid_text(v.value)||!valid_optional_text(v.reason))return false;
    if(static_cast<std::uint8_t>(v.state)>3U)return false;
    const auto parsed=dtv::userland::identity::parse_device_selector(
        std::string_view(v.value.bytes.data(),v.value.length));
    return static_cast<bool>(parsed);
}
template<std::size_t N>
bool valid_candidates(const BArray<DiscoverySelectorCandidate,N>& values) noexcept
{
    if(values.count>N)return false;
    for(std::size_t i=0U;i<values.count;++i)
        if(!valid_candidate(values.items[i]))return false;
    return true;
}
bool valid_function_observation(const UsbFunction& f) noexcept
{
    if(static_cast<std::uint8_t>(f.mode)>2U||
       static_cast<std::uint8_t>(f.serial_state)>4U||
       static_cast<std::uint8_t>(f.identification_reason)>3U||
       !valid_model(f.model))return false;
    if(f.location.usb_path.present){
        const auto& t=f.location.usb_path.value;
        if(!valid_text(t))return false;
        // A location is a canonical portable USB selector path, not an OS
        // device-instance string. Reuse the public selector's grammar.
        std::array<char,detail::kMaxUsbPathBytes+4U> value{};
        if(t.length+4U>value.size())return false;
        value[0]='u';value[1]='s';value[2]='b';value[3]=':';
        for(std::size_t i=0U;i<t.length;++i)value[i+4U]=t.bytes[i];
        if(!dtv::userland::identity::parse_device_selector(
            std::string_view(value.data(),t.length+4U)))return false;
    }
    if(f.serial_state==SerialState::present)
        return f.serial.present&&valid_text(f.serial.value);
    return !f.serial.present;
}
bool valid_identity(const Identity& value) noexcept
{
    if(static_cast<std::uint8_t>(value.serial_state)>2U||
       static_cast<std::uint8_t>(value.uniqueness)>3U)return false;
    if(value.serial_state==IdentitySerialState::present){
        if(!value.serial.present||!valid_text(value.serial.value)||
           value.uniqueness==Uniqueness::not_applicable)return false;
    }else if(value.serial.present)return false;
    if(value.serial_state==IdentitySerialState::absent&&
       value.uniqueness!=Uniqueness::not_applicable)return false;
    if(value.serial_state==IdentitySerialState::unknown&&
       value.uniqueness==Uniqueness::unique_in_scan)return false;
    return true;
}
bool valid_discovery(const DiscoveryResponse& v) noexcept
{
    if(!valid_text(v.producer.product)||!valid_text(v.producer.version)||
       !valid_optional_text(v.producer.core_version)||
       (v.issues.count!=0U&&v.issues.items==nullptr)||
       (v.usb_functions.count!=0U&&v.usb_functions.items==nullptr)||
       (v.enclosures.count!=0U&&v.enclosures.items==nullptr)||
       (v.complete&&v.issues.count!=0U)||
       (!v.complete&&v.issues.count==0U))return false;
    for(std::size_t i=0U;i<v.issues.count;++i)
        if(v.issues.items[i].code==0U||
           dtv::userland::error_symbol(v.issues.items[i].code)==nullptr||
           !valid_text(v.issues.items[i].detail))return false;
    for(std::size_t i=0U;i<v.usb_functions.count;++i){
        const auto& f=v.usb_functions.items[i];
        if(!valid_function_observation(f.observation)||
           !valid_optional_text(f.grouping_reason)||
           !valid_candidates(f.selector_candidates)||
           static_cast<std::uint8_t>(f.grouping)>2U||
           (f.grouping==DiscoveryGroupingState::bound)!=f.enclosure_id.present)return false;
        for(std::size_t j=0U;j<i;++j)
            if(f.observation.id==v.usb_functions.items[j].observation.id)return false;
    }
    for(std::size_t i=0U;i<v.enclosures.count;++i){
        const auto& e=v.enclosures.items[i];
        if(!valid_model(e.descriptor.model)||!valid_identity(e.descriptor.identity)||
           validate_topology(e.topology)!=Error::OK||
           !valid_candidates(e.selector_candidates)||
           e.usb_function_ids.count>e.usb_function_ids.items.size()||
           e.receivers.value.count>e.receivers.value.items.size()||
           e.readers.value.count>e.readers.value.items.size())return false;
        // Topology, not the optional descriptive model name, determines
        // whether published receiver/reader collections are known.
        // Model=null may coexist with a fully observed, known topology.
        const bool known=e.topology.state!=TopologyState::unknown;
        if(known!=e.receivers.present||known!=e.readers.present)return false;
        if(e.topology.state==TopologyState::complete&&
           (!e.topology.expected_function_count.present||
            e.topology.expected_function_count.value!=e.usb_function_ids.count))return false;
        if(e.topology.expected_function_count.present&&
           e.topology.expected_function_count.value<e.usb_function_ids.count)return false;
        if(e.receivers.present){
            if(validate_limits(e.descriptor.limits,e.receivers.value.count)!=Error::OK)
                return false;
        }else if(e.descriptor.limits.max_concurrent_streams.present){
            const auto max=e.descriptor.limits.max_concurrent_streams.value;
            if(max==0U||max>kMaxPublicReceivers)return false;
        }
        for(std::size_t j=0U;j<i;++j)
            if(e.descriptor.id==v.enclosures.items[j].descriptor.id)return false;
        for(std::size_t j=0U;j<e.usb_function_ids.count;++j){
            bool found=false;
            const auto fid=e.usb_function_ids.items[j];
            for(std::size_t k=0U;k<j;++k)
                if(fid==e.usb_function_ids.items[k])return false;
            for(std::size_t k=0U;k<v.usb_functions.count;++k){
                const auto& f=v.usb_functions.items[k];
                if(f.observation.id==fid&&
                   f.grouping==DiscoveryGroupingState::bound&&
                   f.enclosure_id.present&&f.enclosure_id.value==e.descriptor.id)
                    found=true;
            }
            if(!found)return false;
        }
        if(e.receivers.present){
            for(std::size_t j=0U;j<e.receivers.value.count;++j){
                const auto& d=e.receivers.value.items[j];
                if(validate_receiver_descriptor(d)!=Error::OK)return false;
                if(d.binding.present&&validate_binding(d.binding.value,
                    e.usb_function_ids.items.data(),e.usb_function_ids.count)!=Error::OK)
                    return false;
                for(std::size_t k=0U;k<j;++k)
                    if(d.id==e.receivers.value.items[k].id)return false;
            }
        }
        if(e.readers.present){
            for(std::size_t j=0U;j<e.readers.value.count;++j){
                const auto& d=e.readers.value.items[j];
                if(validate_reader_descriptor(d)!=Error::OK)return false;
                if(d.binding.present&&validate_binding(d.binding.value,
                    e.usb_function_ids.items.data(),e.usb_function_ids.count)!=Error::OK)
                    return false;
                for(std::size_t k=0U;k<j;++k)
                    if(d.id==e.readers.value.items[k].id)return false;
            }
        }
    }
    for(std::size_t i=0U;i<v.usb_functions.count;++i){
        const auto& f=v.usb_functions.items[i];
        if(f.grouping!=DiscoveryGroupingState::bound)continue;
        std::size_t matches=0U;
        for(std::size_t j=0U;j<v.enclosures.count;++j){
            const auto& e=v.enclosures.items[j];
            if(e.descriptor.id!=f.enclosure_id.value)continue;
            for(std::size_t k=0U;k<e.usb_function_ids.count;++k)
                if(e.usb_function_ids.items[k]==f.observation.id)++matches;
        }
        if(matches!=1U)return false;
    }
    return true;
}
// The public discovery JSON follows physical ownership.  Wire/internal
// records remain normalized, but consumers should never need to join IDs.
bool discovery_usb_id(Writer& w, std::uint16_t vid, std::uint16_t pid) noexcept
{
    static constexpr char hex[] = "0123456789abcdef";
    return w.ch('"') &&
           w.ch(hex[(vid >> 12U) & 15U]) && w.ch(hex[(vid >> 8U) & 15U]) &&
           w.ch(hex[(vid >> 4U) & 15U]) && w.ch(hex[vid & 15U]) &&
           w.ch(':') &&
           w.ch(hex[(pid >> 12U) & 15U]) && w.ch(hex[(pid >> 8U) & 15U]) &&
           w.ch(hex[(pid >> 4U) & 15U]) && w.ch(hex[pid & 15U]) &&
           w.ch('"');
}

const DiscoveryFunction* find_discovery_function(const DiscoveryResponse& scan,
                                                 std::uint16_t id) noexcept
{
    for (std::size_t i = 0U; i < scan.usb_functions.count; ++i)
        if (scan.usb_functions.items[i].observation.id == id)
            return &scan.usb_functions.items[i];
    return nullptr;
}

bool discovery_systems(Writer& w, Systems systems) noexcept
{
    if (!w.ch('[')) return false;
    bool comma = false;
    if ((systems.mask & 1U) != 0U) {
        if (!w.text("\"ISDB-T\"")) return false;
        comma = true;
    }
    if ((systems.mask & 2U) != 0U) {
        if ((comma && !w.ch(',')) || !w.text("\"ISDB-S\"")) return false;
        comma = true;
    }
    // ISDB-C is a standard name, not a proprietary enum string.  The
    // current wire mask only validates T/S; extending it is a separate task.
    if ((systems.mask & 4U) != 0U) {
        if ((comma && !w.ch(',')) || !w.text("\"ISDB-C\"")) return false;
    }
    return w.ch(']');
}

bool discovery_receiver(Writer& w, const ReceiverDescriptor& receiver,
                        std::size_t device_number) noexcept
{
    if (!receiver.binding.present || !w.ch('{') || !key(w, "receiver") ||
        !w.number(receiver.id) || !w.ch(',') || !key(w, "device") ||
        !w.number(device_number) || !w.ch(',') || !key(w, "local") ||
        !w.number(receiver.binding.value.local_id) || !w.ch(',') ||
        !key(w, "systems") || !discovery_systems(w, receiver.systems))
        return false;
    if ((receiver.capabilities.controls & kControlBitLna) != 0U) {
        if (!w.ch(',') || !key(w, "lna_defaults") || !w.ch('{')) return false;
        for (std::size_t i = 0U; i < receiver.capabilities.lna.entries.count; ++i) {
            const auto& d = receiver.capabilities.lna.entries.items[i];
            if ((i != 0U && !w.ch(',')) ||
                !key(w, d.system == System::isdb_s ? "ISDB-S" : "ISDB-T") ||
                !(d.default_value.present ? boolean(w, d.default_value.value)
                                           : w.text("null"))) return false;
        }
        if (!w.ch('}')) return false;
    }
    return w.ch('}');
}

bool discovery_device(Writer& w, const DiscoveryResponse& scan,
                      const DiscoveryEnclosure& enclosure,
                      std::uint16_t function_id, std::size_t slot) noexcept
{
    const auto* observed = find_discovery_function(scan, function_id);
    if (observed == nullptr) return false;
    const auto& usb = observed->observation;
    if (!w.ch('{') || !key(w, "device") || !w.number(slot + 1U) ||
        !w.ch(',') || !key(w, "usb") || !discovery_usb_id(w, usb.vid, usb.pid) ||
        !w.ch(',') || !key(w, "serial") ||
        !(usb.serial.present ? str(w, usb.serial.value) : w.text("null")) ||
        !w.ch(',') || !key(w, "bus") ||
        !(usb.location.bus.present ? w.number(usb.location.bus.value) : w.text("null")) ||
        !w.ch(',') || !key(w, "address") ||
        !(usb.location.address.present ? w.number(usb.location.address.value) : w.text("null")) ||
        !w.ch(',') || !key(w, "port") ||
        !(usb.location.usb_path.present ? str(w, usb.location.usb_path.value)
                                        : w.text("null"))) return false;
    if (enclosure.receivers.present) {
        if (!w.ch(',') || !key(w, "receivers") || !w.ch('[')) return false;
        bool first = true;
        for (std::size_t i = 0U; i < enclosure.receivers.value.count; ++i) {
            const auto& receiver = enclosure.receivers.value.items[i];
            if (!receiver.binding.present) return false;
            if (receiver.binding.value.function_id != function_id) continue;
            if ((!first && !w.ch(',')) || !discovery_receiver(w, receiver, slot + 1U))
                return false;
            first = false;
        }
        if (!w.ch(']')) return false;
    }
    return w.ch('}');
}

bool discovery_unique(Writer& w, const DiscoveryResponse& scan,
                      const DiscoveryEnclosure& e) noexcept
{
    const auto& serial = e.descriptor.identity.serial;
    if (!serial.present) return w.text("null");
    bool duplicate = false;
    for (std::size_t i = 0U; i < scan.enclosures.count; ++i) {
        const auto& other = scan.enclosures.items[i];
        if (other.descriptor.id == e.descriptor.id ||
            !other.descriptor.identity.serial.present) continue;
        const auto& candidate = other.descriptor.identity.serial.value;
        if (candidate.length == serial.value.length &&
            std::string_view(candidate.bytes.data(), candidate.length) ==
                std::string_view(serial.value.bytes.data(), serial.value.length)) {
            duplicate = true;
            break;
        }
    }
    // Observed duplicates are unambiguously non-unique even on a partial
    // scan. A non-duplicate becomes "true" only after a complete scan.
    return duplicate ? boolean(w, false) :
           scan.complete ? boolean(w, true) : w.text("null");
}

bool discovery_enclosure(Writer& w, const DiscoveryResponse& scan,
                         const DiscoveryEnclosure& e) noexcept
{
    const auto& identity = e.descriptor.identity;
    if (!w.ch('{') || !key(w, "model") ||
        !(e.descriptor.model.present ? str(w, e.descriptor.model.value.name)
                                     : w.text("null")) ||
        !w.ch(',') || !key(w, "serial") ||
        !(identity.serial.present ? str(w, identity.serial.value) : w.text("null")) ||
        !w.ch(',') || !key(w, "serial_unique") || !discovery_unique(w, scan, e))
        return false;

    // A dual-USB enclosure generally has the same VID:PID on both functions.
    // A mixed cold/runtime enclosure has no single VID:PID; each device still
    // carries its observed USB ID.
    const UsbFunction* first_usb = nullptr;
    bool same_usb_id = e.usb_function_ids.count != 0U;
    bool all_runtime = same_usb_id;
    bool any_cold = false;
    for (std::size_t i = 0U; i < e.usb_function_ids.count; ++i) {
        const auto* f = find_discovery_function(scan, e.usb_function_ids.items[i]);
        if (!f) return false;
        const auto& usb = f->observation;
        if (first_usb && (first_usb->vid != usb.vid || first_usb->pid != usb.pid))
            same_usb_id = false;
        if (!first_usb) first_usb = &usb;
        if (usb.mode == Mode::cold) any_cold = true;
        if (usb.mode != Mode::runtime) all_runtime = false;
    }
    if (!w.ch(',') || !key(w, "usb") ||
        !(same_usb_id ? discovery_usb_id(w, first_usb->vid, first_usb->pid)
                       : w.text("null")) ||
        !w.ch(',') || !key(w, "in_loader_mode") ||
        !(any_cold ? boolean(w, true)
                    : all_runtime ? boolean(w, false) : w.text("null")) ||
        !w.ch(',') || !key(w, "devices") || !w.ch('[')) return false;
    for (std::size_t i = 0U; i < e.usb_function_ids.count; ++i) {
        if ((i && !w.ch(',')) ||
            !discovery_device(w, scan, e, e.usb_function_ids.items[i], i))
            return false;
    }
    if (!w.ch(']')) return false;
    if (e.receivers.present) {
        bool lnb_supported = false;
        for (std::size_t i = 0U; i < e.receivers.value.count; ++i) {
            const auto& c = e.receivers.value.items[i].capabilities;
            if ((c.controls & kControlBitLnb) == 0U) continue;
            for (std::size_t j = 0U; j < c.lnb.values.count; ++j)
                if (c.lnb.values.items[j] == 15U) lnb_supported = true;
        }
        if (!w.ch(',') || !key(w, "lnb_15v_supported") ||
            !boolean(w, lnb_supported)) return false;
    }
    if (e.readers.present) {
        if (!w.ch(',') || !key(w, "readers") || !w.ch('[')) return false;
        for (std::size_t i = 0U; i < e.readers.value.count; ++i) {
            const auto& reader = e.readers.value.items[i];
            if (!reader.binding.present) return false;
            bool found = false;
            std::size_t slot = 0U;
            for (; slot < e.usb_function_ids.count; ++slot)
                if (e.usb_function_ids.items[slot] == reader.binding.value.function_id) {
                    found = true;
                    break;
                }
            if (!found || (i && !w.ch(',')) || !w.ch('{') ||
                !key(w, "reader") || !w.number(reader.id) || !w.ch(',') ||
                !key(w, "device") || !w.number(slot + 1U) || !w.ch('}'))
                return false;
        }
        if (!w.ch(']')) return false;
    }
    return w.ch('}');
}

bool discovery_ungrouped(Writer& w, const DiscoveryFunction& f) noexcept
{
    const auto& usb = f.observation;
    if (!w.ch('{') || !key(w, "usb") || !discovery_usb_id(w, usb.vid, usb.pid) ||
        !w.ch(',') || !key(w, "serial") ||
        !(usb.serial.present ? str(w, usb.serial.value) : w.text("null")) ||
        !w.ch(',') || !key(w, "port") ||
        !(usb.location.usb_path.present ? str(w, usb.location.usb_path.value)
                                        : w.text("null")) ||
        !w.ch(',') || !key(w, "bus") ||
        !(usb.location.bus.present ? w.number(usb.location.bus.value) : w.text("null")) ||
        !w.ch(',') || !key(w, "address") ||
        !(usb.location.address.present ? w.number(usb.location.address.value)
                                       : w.text("null"))) return false;
    return w.ch('}');
}

bool emit_discovery(const DiscoveryResponse& scan, Writer& w) noexcept
{
    if (!w.ch('{') || !key(w, "schema") ||
        !w.text("{\"name\":\"userland.discovery\",\"major\":1,\"minor\":0}") ||
        !w.ch(',') || !key(w, "producer") || !w.ch('{') ||
        !key(w, "product") || !str(w, scan.producer.product) ||
        !w.ch(',') || !key(w, "version") || !str(w, scan.producer.version))
        return false;
    if (scan.producer.core_version.present) {
        if (!w.ch(',') || !key(w, "core_version") ||
            !str(w, scan.producer.core_version.value)) return false;
    }
    if (!w.ch('}') || !w.ch(',') || !key(w, "scan") || !w.ch('{') ||
        !key(w, "complete") || !boolean(w, scan.complete) ||
        !w.ch(',') || !key(w, "issues") || !w.ch('[')) return false;
    for (std::size_t i = 0U; i < scan.issues.count; ++i) {
        const auto& issue = scan.issues.items[i];
        if ((i && !w.ch(',')) || !w.ch('{') || !key(w, "code") ||
            !w.number(issue.code) || !w.ch(',') || !key(w, "detail") ||
            !str(w, issue.detail) || !w.ch('}')) return false;
    }
    if (!w.ch(']') || !w.ch('}') || !w.ch(',') ||
        !key(w, "enclosures") || !w.ch('[')) return false;
    bool have_previous = false;
    std::uint16_t previous = 0U;
    for (std::size_t done = 0; done < scan.enclosures.count; ++done) {
        bool found = false;
        std::size_t next = 0U;
        for (std::size_t i = 0U; i < scan.enclosures.count; ++i) {
            const auto id = scan.enclosures.items[i].descriptor.id;
            if ((!have_previous || id > previous) &&
                (!found || id < scan.enclosures.items[next].descriptor.id)) {
                next = i;
                found = true;
            }
        }
        if (!found || (done && !w.ch(',')) ||
            !discovery_enclosure(w, scan, scan.enclosures.items[next])) return false;
        previous = scan.enclosures.items[next].descriptor.id;
        have_previous = true;
    }
    if (!w.ch(']') || !w.ch(',') ||
        !key(w, "ungrouped_usb_devices") || !w.ch('[')) return false;
    bool first = true;
    for (std::size_t i = 0U; i < scan.usb_functions.count; ++i) {
        if (scan.usb_functions.items[i].grouping == DiscoveryGroupingState::bound)
            continue;
        if ((!first && !w.ch(',')) ||
            !discovery_ungrouped(w, scan.usb_functions.items[i]))
            return false;
        first = false;
    }
    return w.text("]}\n");
}

}  // namespace

Result<std::size_t> status_json_size(const StatusResponse& value,const StatusContext& context) noexcept
{
    const Error valid=validate_status_response(value,context);if(valid!=Error::OK)return Result<std::size_t>::failure(valid);
    Writer count({nullptr,0U});if(!emit(value,count))return Result<std::size_t>::failure(Error::RESOURCE_LIMIT);
    return Result<std::size_t>::success(count.size());
}

Result<std::size_t> encode_status_json(const StatusResponse& value,const StatusContext& context,MutableByteView output) noexcept
{
    const Error valid=validate_status_response(value,context);if(valid!=Error::OK)return Result<std::size_t>::failure(valid);
    if(output.data==nullptr&&output.size!=0U)return Result<std::size_t>::failure(Error::INVALID_ARGUMENT);
    Writer count({nullptr,0U});if(!emit(value,count))return Result<std::size_t>::failure(Error::RESOURCE_LIMIT);
    if(output.size<count.size())return Result<std::size_t>::failure(Error::BUFFER_TOO_SMALL);
    Writer write(output);if(!emit(value,write)||write.size()!=count.size())return Result<std::size_t>::failure(Error::RESOURCE_LIMIT);
    return Result<std::size_t>::success(write.size());
}


Result<std::size_t> discovery_json_size(const DiscoveryResponse& value) noexcept
{
    if(!valid_discovery(value))
        return Result<std::size_t>::failure(Error::INVALID_ARGUMENT);
    Writer count({nullptr,0U});
    if(!emit_discovery(value,count))
        return Result<std::size_t>::failure(Error::RESOURCE_LIMIT);
    return Result<std::size_t>::success(count.size());
}

Result<std::size_t> encode_discovery_json(const DiscoveryResponse& value,
                                          MutableByteView output) noexcept
{
    if(!valid_discovery(value))
        return Result<std::size_t>::failure(Error::INVALID_ARGUMENT);
    if(output.data==nullptr&&output.size!=0U)
        return Result<std::size_t>::failure(Error::INVALID_ARGUMENT);
    Writer count({nullptr,0U});
    if(!emit_discovery(value,count))
        return Result<std::size_t>::failure(Error::RESOURCE_LIMIT);
    if(output.size<count.size())
        return Result<std::size_t>::failure(Error::BUFFER_TOO_SMALL);
    Writer writer(output);
    if(!emit_discovery(value,writer)||writer.size()!=count.size())
        return Result<std::size_t>::failure(Error::RESOURCE_LIMIT);
    return Result<std::size_t>::success(writer.size());
}



namespace {

// This catalog intentionally carries only static build and profile facts.
// One model is not a discovered USB enclosure; available_in_build is not a
// claim that hardware has been tested or is currently connected.
bool valid_models(const ModelsResponse& value) noexcept
{
    if (!valid_text(value.producer.product) || !valid_text(value.producer.version) ||
        !valid_optional_text(value.producer.core_version) ||
        value.models.count > kMaxCatalogModels ||
        (value.models.count != 0U && value.models.items == nullptr)) return false;

    for (std::size_t i = 0U; i < value.models.count; ++i) {
        const auto& e = value.models.items[i];
        if (!valid_text(e.model.id) || !valid_text(e.model.name) ||
            e.usb_ids.count > kMaxCatalogUsbIds ||
            (e.nominal_receiver_count.present && e.nominal_receiver_count.value == 0U) ||
            (e.nominal_usb_function_count.present &&
             e.nominal_usb_function_count.value == 0U) ||
            e.receivers.value.count > kMaxPublicReceivers) return false;

        const auto id = std::string_view(e.model.id.bytes.data(), e.model.id.length);
        for (std::size_t j = 0U; j < i; ++j) {
            const auto& preceding = value.models.items[j].model.id;
            if (id == std::string_view(preceding.bytes.data(), preceding.length))
                return false;
        }
        for (std::size_t j = 0U; j < e.usb_ids.count; ++j) {
            const auto& u = e.usb_ids.items[j];
            if (static_cast<std::uint8_t>(u.mode) > 2U) return false;
            for (std::size_t k = 0U; k < j; ++k) {
                const auto& prev = e.usb_ids.items[k];
                if (u.vid == prev.vid && u.pid == prev.pid && u.mode == prev.mode)
                    return false;
            }
        }

        if (!e.available_in_build) {
            if (!e.receivers.present || e.receivers.value.count != 0U ||
                !e.max_concurrent_streams.present ||
                e.max_concurrent_streams.value != 0U) return false;
            continue;
        }
        if (!e.receivers.present) {
            // Unknown public composition must never be silently written as [].
            if (e.max_concurrent_streams.present) return false;
            continue;
        }
        if (e.max_concurrent_streams.present &&
            (e.max_concurrent_streams.value > e.receivers.value.count ||
             (e.max_concurrent_streams.value == 0U &&
              e.receivers.value.count != 0U))) return false;

        for (std::size_t j = 0U; j < e.receivers.value.count; ++j) {
            const auto& r = e.receivers.value.items[j];
            if (r.systems.mask == 0U || (r.systems.mask & ~3U) != 0U)
                return false;
            for (std::size_t k = 0U; k < j; ++k)
                if (e.receivers.value.items[k].id == r.id) return false;
        }
    }
    return true;
}

bool hex_id(Writer& w, std::uint16_t v) noexcept
{
    static constexpr char hex[] = "0123456789abcdef";
    return w.ch('"') && w.ch(hex[(v >> 12U) & 15U]) &&
           w.ch(hex[(v >> 8U) & 15U]) &&
           w.ch(hex[(v >> 4U) & 15U]) && w.ch(hex[v & 15U]) &&
           w.ch('"');
}

bool optional_count(Writer& w, const Optional<std::uint16_t>& v) noexcept
{
    return v.present ? w.number(v.value) : w.text("null");
}

bool emit_model(Writer& w, const ModelCatalogEntry& m) noexcept
{
    if (!w.ch('{') || !key(w, "id") || !str(w, m.model.id) ||
        !w.ch(',') || !key(w, "name") || !str(w, m.model.name) ||
        !w.ch(',') || !key(w, "usb_ids") || !w.ch('[')) return false;
    for (std::size_t i = 0U; i < m.usb_ids.count; ++i) {
        const auto& u = m.usb_ids.items[i];
        const char* phase = mode(u.mode);
        if ((i != 0U && !w.ch(',')) || !w.ch('{') ||
            !key(w, "vid") || !hex_id(w, u.vid) ||
            !w.ch(',') || !key(w, "pid") || !hex_id(w, u.pid) ||
            !w.ch(',') || !key(w, "mode") ||
            !w.string(phase, length(phase)) || !w.ch('}')) return false;
    }
    if (!w.ch(']') || !w.ch(',') || !key(w, "nominal") ||
        !w.ch('{') || !key(w, "receiver_count") ||
        !optional_count(w, m.nominal_receiver_count) ||
        !w.ch(',') || !key(w, "usb_function_count") ||
        !optional_count(w, m.nominal_usb_function_count) ||
        !w.ch('}') || !w.ch(',') || !key(w, "implementation") ||
        !w.ch('{') || !key(w, "available_in_build") ||
        !boolean(w, m.available_in_build) || !w.ch(',') ||
        !key(w, "receivers")) return false;
    if (!m.receivers.present) {
        if (!w.text("null")) return false;
    } else {
        if (!w.ch('[')) return false;
        for (std::size_t i = 0U; i < m.receivers.value.count; ++i) {
            const auto& r = m.receivers.value.items[i];
            if ((i != 0U && !w.ch(',')) ||
                !w.ch('{') || !key(w, "id") || !w.quoted_number(r.id) ||
                !w.ch(',') || !key(w, "systems") || !w.ch('[')) return false;
            bool separator = false;
            if ((r.systems.mask & 1U) != 0U) {
                if (!w.text("\"isdb-t\"")) return false;
                separator = true;
            }
            if ((r.systems.mask & 2U) != 0U) {
                if ((separator && !w.ch(',')) ||
                    !w.text("\"isdb-s\"")) return false;
            }
            if (!w.text("]}")) return false;
        }
        if (!w.ch(']')) return false;
    }
    return w.ch(',') && key(w, "max_concurrent_streams") &&
           optional_count(w, m.max_concurrent_streams) && w.text("}}");
}

bool emit_models(const ModelsResponse& v, Writer& w) noexcept
{
    if (!w.text("{\"schema\":{\"name\":\"userland.models\",\"major\":1,\"minor\":0},") ||
        !key(w, "producer") || !producer(w, v.producer) ||
        !w.ch(',') || !key(w, "models") || !w.ch('[')) return false;

    // Stable model ID order without allocating an auxiliary vector.
    std::string_view previous;
    for (std::size_t emitted = 0U; emitted < v.models.count; ++emitted) {
        bool found = false;
        std::size_t next = 0U;
        for (std::size_t i = 0U; i < v.models.count; ++i) {
            const auto& id = v.models.items[i].model.id;
            const auto name = std::string_view(id.bytes.data(), id.length);
            if ((!previous.empty() && name <= previous) ||
                (found && name >= std::string_view(
                    v.models.items[next].model.id.bytes.data(),
                    v.models.items[next].model.id.length))) continue;
            next = i;
            found = true;
        }
        if (!found || (emitted != 0U && !w.ch(',')) ||
            !emit_model(w, v.models.items[next])) return false;
        const auto& id = v.models.items[next].model.id;
        previous = std::string_view(id.bytes.data(), id.length);
    }
    return w.text("]}\n");
}
}  // namespace

Result<std::size_t> models_json_size(const ModelsResponse& v) noexcept
{
    if (!valid_models(v))
        return Result<std::size_t>::failure(Error::INVALID_ARGUMENT);
    Writer count({nullptr, 0U});
    if (!emit_models(v, count))
        return Result<std::size_t>::failure(Error::RESOURCE_LIMIT);
    return Result<std::size_t>::success(count.size());
}

Result<std::size_t> encode_models_json(const ModelsResponse& v,
                                       MutableByteView output) noexcept
{
    if (!valid_models(v) || (output.data == nullptr && output.size != 0U))
        return Result<std::size_t>::failure(Error::INVALID_ARGUMENT);
    Writer count({nullptr, 0U});
    if (!emit_models(v, count))
        return Result<std::size_t>::failure(Error::RESOURCE_LIMIT);
    if (output.size < count.size())
        return Result<std::size_t>::failure(Error::BUFFER_TOO_SMALL);
    Writer actual(output);
    if (!emit_models(v, actual) || actual.size() != count.size())
        return Result<std::size_t>::failure(Error::RESOURCE_LIMIT);
    return Result<std::size_t>::success(actual.size());
}


// The CLI surface is intentionally much smaller than the private
// ModelCatalogEntry / old userland.models JSON proposal. No model capabilities,
// receiver counts, status or runtime observations leak into --models.
namespace {
bool valid_supported_models(const SupportedModels& list) noexcept
{
    if (list.models.count > kMaxCatalogModels ||
        (list.models.count && list.models.items == nullptr)) return false;
    for (std::size_t i = 0; i < list.models.count; ++i) {
        const auto& item = list.models.items[i];
        if (item.name.length == 0U || item.name.length > item.name.bytes.size() ||
            item.usb_ids.count == 0U || item.usb_ids.count > kMaxCatalogUsbIds)
            return false;
        for (std::size_t j = 0; j < item.name.length; ++j) {
            const auto c = static_cast<unsigned char>(item.name.bytes[j]);
            // Source-backed model names can include spaces (PX-W3U3 V2).
            // Reject controls/newlines and non-ASCII bytes; never treat USB
            // descriptor strings as safe static model names.
            if (c < 0x20U || c >= 0x7fU) return false;
        }
        const std::string_view name(item.name.bytes.data(), item.name.length);
        for (std::size_t j = 0; j < i; ++j) {
            const auto& old = list.models.items[j].name;
            if (name == std::string_view(old.bytes.data(), old.length)) return false;
        }
        for (std::size_t j = 0; j < item.usb_ids.count; ++j) {
            const auto usb = item.usb_ids.items[j];
            for (std::size_t k = 0; k < j; ++k) {
                const auto other = item.usb_ids.items[k];
                if (usb.vid == other.vid && usb.pid == other.pid) return false;
            }
        }
    }
    return true;
}
bool hex_raw(Writer& writer, std::uint16_t number) noexcept
{
    static constexpr char digits[] = "0123456789abcdef";
    return writer.ch(digits[(number >> 12U) & 0xfU]) &&
           writer.ch(digits[(number >> 8U) & 0xfU]) &&
           writer.ch(digits[(number >> 4U) & 0xfU]) &&
           writer.ch(digits[number & 0xfU]);
}
bool emit_supported_models(const SupportedModels& models, Writer& out) noexcept
{
    std::string_view previous;
    for (std::size_t done = 0; done < models.models.count; ++done) {
        std::size_t chosen = 0;
        bool found = false;
        for (std::size_t i = 0; i < models.models.count; ++i) {
            const auto& n = models.models.items[i].name;
            const std::string_view current(n.bytes.data(), n.length);
            if ((!previous.empty() && current <= previous) ||
                (found && current >= std::string_view(
                    models.models.items[chosen].name.bytes.data(),
                    models.models.items[chosen].name.length))) continue;
            chosen = i;
            found = true;
        }
        if (!found) return false;
        const auto& item = models.models.items[chosen];
        for (std::size_t k = 0; k < item.name.length; ++k) {
            if (!out.ch(item.name.bytes[k])) return false;
        }
        for (std::size_t i = 0; i < item.usb_ids.count; ++i) {
            const auto& usb = item.usb_ids.items[i];
            if (!out.ch(' ') || !hex_raw(out, usb.vid) ||
                !out.ch(':') || !hex_raw(out, usb.pid)) return false;
        }
        if (!out.ch('\n')) return false;
        previous = std::string_view(item.name.bytes.data(), item.name.length);
    }
    return true;
}
}  // namespace

Result<std::size_t> models_text_size(const SupportedModels& models) noexcept
{
    if (!valid_supported_models(models))
        return Result<std::size_t>::failure(Error::INVALID_ARGUMENT);
    Writer count({nullptr, 0U});
    if (!emit_supported_models(models, count))
        return Result<std::size_t>::failure(Error::RESOURCE_LIMIT);
    return Result<std::size_t>::success(count.size());
}

Result<std::size_t> encode_models_text(const SupportedModels& models,
                                       MutableByteView output) noexcept
{
    if (!valid_supported_models(models) || (output.data == nullptr && output.size))
        return Result<std::size_t>::failure(Error::INVALID_ARGUMENT);
    Writer count({nullptr, 0U});
    if (!emit_supported_models(models, count))
        return Result<std::size_t>::failure(Error::RESOURCE_LIMIT);
    if (output.size < count.size())
        return Result<std::size_t>::failure(Error::BUFFER_TOO_SMALL);
    Writer writer(output);
    if (!emit_supported_models(models, writer) || writer.size() != count.size())
        return Result<std::size_t>::failure(Error::RESOURCE_LIMIT);
    return Result<std::size_t>::success(writer.size());
}

}  // namespace dtv::userland::wire
