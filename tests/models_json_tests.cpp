// SPDX-License-Identifier: MIT
#include <dtv/userland/json.h>
#include <dtv/userland/error.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
namespace wire = dtv::userland::wire;
using dtv::userland::Error;
int errors = 0;
#define CHECK(c) do { if (!(c)) { ++errors; std::fprintf(stderr, \
    "models JSON check failed %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (false)

template <std::size_t N>
void assign(wire::WString<N>& target, const char* text)
{
    const std::size_t len = std::strlen(text);
    if (len > target.bytes.size()) { CHECK(false); return; }
    target.length = static_cast<std::uint16_t>(len);
    std::memcpy(target.bytes.data(), text, len);
}

void ordinary_catalog()
{
    wire::ModelCatalogEntry models[2]{};
    // Deliberately unsorted input: catalog output order is lexical by model ID.
    auto& on = models[0];
    assign(on.model.id, "z-available");
    assign(on.model.name, "Receiver & card");
    on.usb_ids.count = 1U;
    on.usb_ids.items[0] = {0x0511U, 0x084aU, wire::Mode::runtime};
    on.nominal_receiver_count = {true, 2U};
    on.nominal_usb_function_count = {true, 1U};
    on.available_in_build = true;
    on.receivers.present = true;
    on.receivers.value.count = 1U;
    on.receivers.value.items[0].id = 0U;
    on.receivers.value.items[0].systems.mask = 3U;
    on.max_concurrent_streams = {true, 1U};

    auto& off = models[1];
    assign(off.model.id, "a-disabled");
    assign(off.model.name, "Not in this build");
    off.usb_ids.count = 1U;
    off.usb_ids.items[0] = {0x1738U, 0x5211U, wire::Mode::cold};
    off.nominal_receiver_count = {true, 4U};
    off.nominal_usb_function_count = {true, 2U};
    off.available_in_build = false;
    off.receivers.present = true;
    off.receivers.value.count = 0U;
    off.max_concurrent_streams = {true, 0U};

    wire::ModelsResponse document{};
    assign(document.producer.product, "test-userland");
    assign(document.producer.version, "0.0.1");
    document.models = {models, 2U};
    const auto size = wire::models_json_size(document);
    CHECK(size);
    if (!size) return;
    std::vector<std::uint8_t> bytes(size.value(), 0xa5U);
    auto written = wire::encode_models_json(document, {bytes.data(), bytes.size()});
    CHECK(written && written.value() == size.value());
    if (!written) return;
    const std::string output(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    CHECK(output.find("\"name\":\"userland.models\"") != std::string::npos);
    CHECK(output.find("\"models\":[{\"id\":\"a-disabled\"") != std::string::npos);
    CHECK(output.find("\"id\":\"z-available\"") != std::string::npos);
    CHECK(output.find("\"mode\":\"cold\"") != std::string::npos);
    CHECK(output.find("\"pid\":\"084a\"") != std::string::npos);
    CHECK(output.find("\"systems\":[\"isdb-t\",\"isdb-s\"]") != std::string::npos);
    CHECK(output.find("\"available_in_build\":false,\"receivers\":[]") != std::string::npos);
    CHECK(output.back() == '\n');

    std::vector<std::uint8_t> short_output(size.value(), 0x55U);
    const auto short_write = wire::encode_models_json(document,
            {short_output.data(), short_output.size() - 1U});
    CHECK(!short_write && short_write.error() == Error::BUFFER_TOO_SMALL);
    CHECK(short_output.front() == 0x55U && short_output.back() == 0x55U);

    // Invalid population combination must not be misrepresented as zero.
    off.receivers.present = false;
    auto invalid = wire::encode_models_json(document, {bytes.data(), bytes.size()});
    CHECK(!invalid && invalid.error() == Error::INVALID_ARGUMENT);
}

void unknown_and_empty()
{
    wire::ModelCatalogEntry one{};
    assign(one.model.id, "unknown");
    assign(one.model.name, "Unknown runtime");
    one.available_in_build = true;
    // No receiver inventory or max streams was verified.
    wire::ModelsResponse doc{};
    assign(doc.producer.product, "test-userland");
    assign(doc.producer.version, "0.0.1");
    doc.models = {&one, 1U};
    const auto size = wire::models_json_size(doc);
    CHECK(size);
    if (!size) return;
    std::vector<std::uint8_t> out(size.value());
    CHECK(wire::encode_models_json(doc, {out.data(), out.size()}));
    const std::string text(reinterpret_cast<const char*>(out.data()), out.size());
    CHECK(text.find("\"receiver_count\":null") != std::string::npos);
    CHECK(text.find("\"receivers\":null") != std::string::npos);
    CHECK(text.find("\"max_concurrent_streams\":null") != std::string::npos);

    one.receivers.present = true;
    one.receivers.value.count = 1U;
    one.receivers.value.items[0].systems.mask = 0U;  // Invalid system mask.
    CHECK(!wire::models_json_size(doc));
}

}  // namespace

int main()
{
    ordinary_catalog();
    unknown_and_empty();
    if (errors) std::fprintf(stderr, "models_json_tests failures=%d\n", errors);
    return errors == 0 ? 0 : 1;
}
