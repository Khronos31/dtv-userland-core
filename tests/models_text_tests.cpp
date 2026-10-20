// SPDX-License-Identifier: MIT
#include <dtv/userland/json.h>
#include <dtv/userland/error.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace w = dtv::userland::wire;
int bad = 0;
#define CHECK(v) do { if (!(v)) {++bad; std::fprintf(stderr, "CHECK failed at %d: %s\n", __LINE__, #v);} }while(false)

void put(w::WString<128>& field, const char* name)
{
    field.length = static_cast<std::uint16_t>(std::strlen(name));
    std::memcpy(field.bytes.data(), name, field.length);
}

int main()
{
    w::SupportedModel models[2]{};
    put(models[0].name, "PX-W3U3");
    models[0].usb_ids.count = 2;
    models[0].usb_ids.items[0] = {0x1738, 0x5211};
    models[0].usb_ids.items[1] = {0x0b06, 0x0005};
    put(models[1].name, "PX-Q3U4");
    models[1].usb_ids.count = 1;
    models[1].usb_ids.items[0] = {0x0511, 0x084a};
    w::SupportedModels doc{{models, 2}};
    const auto size = w::models_text_size(doc);
    CHECK(size);
    if (size) {
        std::vector<std::uint8_t> bytes(size.value(), 0xa5U);
        const auto written = w::encode_models_text(doc, {bytes.data(), bytes.size()});
        CHECK(written && written.value() == size.value());
        if (written) {
            std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            CHECK(text == "PX-Q3U4 0511:084a\nPX-W3U3 1738:5211 0b06:0005\n");
        }
        bytes.assign(size.value(), 0x55);
        const auto short_out = w::encode_models_text(doc, {bytes.data(), bytes.size() - 1});
        CHECK(!short_out && short_out.error() == dtv::userland::Error::BUFFER_TOO_SMALL);
        CHECK(bytes.front() == 0x55 && bytes.back() == 0x55);
    }
    models[1].usb_ids.items[0] = models[0].usb_ids.items[0];
    CHECK(w::models_text_size(doc)); // shared loader IDs across distinct models are legal.
    put(models[1].name, "PX-W3U3");
    CHECK(!w::models_text_size(doc)); // duplicate names are ambiguous.
    put(models[1].name, "PX-Q3U4");
    models[1].name.bytes[0] = '\n';
    CHECK(!w::models_text_size(doc)); // cannot inject extra lines.
    models[1].name.bytes[0] = 'P';
    models[1].usb_ids.count = 2;
    models[1].usb_ids.items[1] = models[1].usb_ids.items[0];
    CHECK(!w::models_text_size(doc)); // repeated ID within same model.
    return bad ? 1 : 0;
}
