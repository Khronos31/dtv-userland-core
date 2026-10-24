// SPDX-License-Identifier: GPL-2.0-only
#ifndef DTV_USERLAND_WINDOWS_WINSCARD_PRODUCT_H
#define DTV_USERLAND_WINDOWS_WINSCARD_PRODUCT_H

namespace px4::userland::windows_card {

struct Product final {
    const char* slug;
    const wchar_t* namespace_component;
    const char* reader_component;
};

inline constexpr Product kProducts[] = {
    {"px4-userland", L"px4-userland", "px4"},
    {"asicen-userland", L"asicen-userland", "asicen"},
};
inline constexpr char kProductVersion[] = "v1";
inline constexpr wchar_t kProductVersionW[] = L"v1";
inline constexpr char kReaderPrefix[] = "DTV userland (";
inline constexpr wchar_t kRuntimeEnv[] = L"DTV_USERLAND_RUNTIME_DIR";

}  // namespace px4::userland::windows_card

#endif  // DTV_USERLAND_WINDOWS_WINSCARD_PRODUCT_H
