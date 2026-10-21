// SPDX-License-Identifier: MIT
#include <dtv/userland/discovery_json.h>

#include <cstddef>
#include <cstdint>

namespace dtv::userland::wire {
namespace {

struct Writer final {
    char* output = nullptr;
    std::size_t position = 0U;

    void put(char value) noexcept
    {
        if (output != nullptr) output[position] = value;
        ++position;
    }

    void text(const char* value) noexcept
    {
        while (*value != '\0') put(*value++);
    }

    void number(std::uint32_t value) noexcept
    {
        char digits[10];
        std::size_t count = 0U;
        do {
            digits[count++] = static_cast<char>('0' + (value % 10U));
            value /= 10U;
        } while (value != 0U);
        while (count != 0U) put(digits[--count]);
    }

    void string(const char* value, std::size_t length) noexcept
    {
        put('"');
        for (std::size_t i = 0U; i < length; ++i) {
            const unsigned char c = static_cast<unsigned char>(value[i]);
            switch (c) {
            case '"': text("\\\""); break;
            case '\\': text("\\\\"); break;
            case '\b': text("\\b"); break;
            case '\f': text("\\f"); break;
            case '\n': text("\\n"); break;
            case '\r': text("\\r"); break;
            case '\t': text("\\t"); break;
            default:
                if (c < 0x20U) {
                    static constexpr char hex[] = "0123456789abcdef";
                    text("\\u00"); put(hex[c >> 4U]); put(hex[c & 0x0fU]);
                } else {
                    put(static_cast<char>(c));
                }
            }
        }
        put('"');
    }

    template <std::size_t N>
    void string(const WString<N>& value) noexcept { string(value.bytes.data(), value.length); }

};

bool valid_text(const char* value, std::size_t length, std::size_t capacity) noexcept
{
    if (length > capacity || !detail::strict_utf8(
            reinterpret_cast<const std::uint8_t*>(value), length)) return false;
    for (std::size_t i = 0U; i < length; ++i) {
        if (value[i] == '\0') return false;
    }
    return true;
}

template <std::size_t N>
bool valid_text(const WString<N>& value) noexcept
{
    return valid_text(value.bytes.data(), value.length, value.bytes.size());
}

bool valid_systems(Systems systems) noexcept
{
    return systems.mask != 0U && (systems.mask & ~0x03U) == 0U;
}

bool valid(const DiscoveryScan& value) noexcept
{
    if (!valid_text(value.product) || !valid_text(value.version) ||
        (value.enclosure_count != 0U && value.enclosures == nullptr)) return false;
    for (std::size_t i = 0U; i < value.enclosure_count; ++i) {
        const auto& enclosure = value.enclosures[i];
        for (std::size_t before = 0U; before < i; ++before) {
            if (value.enclosures[before].enclosure == enclosure.enclosure) return false;
        }
        if (enclosure.device_count != 0U && enclosure.devices == nullptr) return false;
        if (enclosure.model.present && !valid_text(enclosure.model.value.bytes.data(),
                enclosure.model.value.length, enclosure.model.value.bytes.size())) return false;
        if (enclosure.serial.present && (enclosure.serial.value.length == 0U ||
                !valid_text(enclosure.serial.value.bytes.data(), enclosure.serial.value.length,
                            enclosure.serial.value.bytes.size()))) return false;
        if (!enclosure.readers_known && enclosure.reader_count != 0U) return false;
        if (enclosure.readers_known && enclosure.reader_count != 0U &&
            enclosure.readers == nullptr) return false;
        for (std::size_t j = 0U; j < enclosure.reader_count; ++j) {
            const auto& reader = enclosure.readers[j];
            bool device_found = false;
            for (std::size_t k = 0U; k < enclosure.device_count; ++k) {
                if (enclosure.devices[k].device == reader.device) device_found = true;
            }
            if (!device_found) return false;
            for (std::size_t before = 0U; before < j; ++before) {
                if (enclosure.readers[before].reader == reader.reader) return false;
            }
        }
        for (std::size_t j = 0U; j < enclosure.device_count; ++j) {
            const auto& device = enclosure.devices[j];
            for (std::size_t before = 0U; before < j; ++before) {
                if (enclosure.devices[before].device == device.device) return false;
            }
            if (!device.receivers_known && device.receiver_count != 0U) return false;
            if (device.receivers_known && device.receiver_count != 0U && device.receivers == nullptr) return false;
            for (std::size_t k = 0U; k < device.receiver_count; ++k) {
                const auto& receiver = device.receivers[k];
                if (!valid_systems(receiver.systems) || receiver.device != device.device) return false;
                for (std::size_t before = 0U; before < k; ++before) {
                    if (device.receivers[before].local == receiver.local ||
                        device.receivers[before].receiver == receiver.receiver) return false;
                }
                for (std::size_t prior_device = 0U; prior_device <= j;
                     ++prior_device) {
                    const std::size_t receiver_limit = prior_device == j ? k :
                        enclosure.devices[prior_device].receiver_count;
                    for (std::size_t prior_receiver = 0U;
                         prior_receiver < receiver_limit; ++prior_receiver) {
                        if (enclosure.devices[prior_device].receivers[prior_receiver].receiver ==
                            receiver.receiver) return false;
                    }
                }
            }
        }
    }
    return true;
}

bool serial_duplicate(const DiscoveryScan& value, std::size_t index) noexcept
{
    const auto& enclosure = value.enclosures[index];
    if (!enclosure.serial.present) return false;
    for (std::size_t i = 0U; i < value.enclosure_count; ++i) {
        if (i != index && value.enclosures[i].serial.present &&
            value.enclosures[i].serial.value.length == enclosure.serial.value.length) {
            bool equal = true;
            for (std::size_t j = 0U; j < enclosure.serial.value.length; ++j) {
                if (enclosure.serial.value.bytes[j] != value.enclosures[i].serial.value.bytes[j]) {
                    equal = false;
                    break;
                }
            }
            if (equal) return true;
        }
    }
    return false;
}

void write(const DiscoveryScan& value, Writer& out) noexcept
{
    out.text("{\"schema\":{\"name\":\"userland.discovery\",\"major\":1,\"minor\":0},\"producer\":{\"product\":");
    out.string(value.product);
    out.text(",\"version\":"); out.string(value.version);
    out.text("},\"scan_complete\":");
    out.text(value.complete ? "true" : "false");
    out.text(",\"enclosures\":[");
    for (std::size_t i = 0U; i < value.enclosure_count; ++i) {
        if (i != 0U) out.put(',');
        const auto& enclosure = value.enclosures[i];
        out.text("{\"enclosure\":"); out.number(enclosure.enclosure);
        out.text(",\"model\":");
        if (enclosure.model.present) out.string(enclosure.model.value); else out.text("null");
        out.text(",\"serial\":");
        if (enclosure.serial.present) out.string(enclosure.serial.value); else out.text("null");
        out.text(",\"serial_unique\":");
        if (!enclosure.serial.present) out.text("null");
        else if (serial_duplicate(value, i)) out.text("false");
        else out.text(value.complete ? "true" : "null");
        out.text(",\"lnb_15v_supported\":");
        if (enclosure.lnb_15v_supported.present)
            out.text(enclosure.lnb_15v_supported.value ? "true" : "false");
        else out.text("null");
        if (enclosure.readers_known) {
            out.text(",\"readers\":[");
            for (std::size_t j = 0U; j < enclosure.reader_count; ++j) {
                if (j != 0U) out.put(',');
                out.text("{\"reader\":"); out.number(enclosure.readers[j].reader);
                out.text(",\"device\":"); out.number(enclosure.readers[j].device);
                out.put('}');
            }
            out.put(']');
        }
        out.text(",\"devices\":[");
        for (std::size_t j = 0U; j < enclosure.device_count; ++j) {
            if (j != 0U) out.put(',');
            const auto& device = enclosure.devices[j];
            out.text("{\"device\":"); out.number(device.device);
            out.text(",\"usb\":\"");
            static constexpr char hex[] = "0123456789abcdef";
            for (int shift = 12; shift >= 0; shift -= 4) out.put(hex[(device.vid >> shift) & 0x0fU]);
            out.put(':');
            for (int shift = 12; shift >= 0; shift -= 4) out.put(hex[(device.pid >> shift) & 0x0fU]);
            out.put('"');
            if (device.receivers_known) {
                out.text(",\"receivers\":[");
                for (std::size_t k = 0U; k < device.receiver_count; ++k) {
                    if (k != 0U) out.put(',');
                    const auto& receiver = device.receivers[k];
                    out.text("{\"receiver\":"); out.number(receiver.receiver);
                    out.text(",\"device\":"); out.number(receiver.device);
                    out.text(",\"local\":"); out.number(receiver.local);
                    out.text(",\"systems\":[");
                    bool comma = false;
                    if ((receiver.systems.mask & 0x01U) != 0U) { out.text("\"ISDB-T\""); comma = true; }
                    if ((receiver.systems.mask & 0x02U) != 0U) { if (comma) out.put(','); out.text("\"ISDB-S\""); }
                    out.text("]}");
                }
                out.put(']');
            }
            out.put('}');
        }
        out.text("]}");
    }
    out.text("]}\n");
}

}  // namespace

Result<std::size_t> discovery_json_size(const DiscoveryScan& value) noexcept
{
    if (!valid(value)) return Result<std::size_t>::failure(Error::INVALID_ARGUMENT);
    Writer writer;
    write(value, writer);
    return Result<std::size_t>::success(writer.position);
}

Result<std::size_t> encode_discovery_json(const DiscoveryScan& value,
                                          MutableByteView output) noexcept
{
    const auto size = discovery_json_size(value);
    if (!size) return Result<std::size_t>::failure(size.error());
    if (output.data == nullptr || output.size < size.value()) {
        return Result<std::size_t>::failure(Error::BUFFER_TOO_SMALL);
    }
    Writer writer{reinterpret_cast<char*>(output.data), 0U};
    write(value, writer);
    return Result<std::size_t>::success(writer.position);
}

}  // namespace dtv::userland::wire
