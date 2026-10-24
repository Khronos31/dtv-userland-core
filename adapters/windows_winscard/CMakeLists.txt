# SPDX-License-Identifier: GPL-2.0-only
#
# Optional, build-tree-only GPL-2.0-only Windows WinSCard adapter.  The same
# product-independent DLL is used by every product that publishes a CARD reader.

if(NOT WIN32)
    message(FATAL_ERROR
        "DTV_USERLAND_BUILD_WINDOWS_WINSCARD requires a Windows target")
endif()

if(NOT TARGET dtv::control OR NOT TARGET dtv::host_runtime)
    message(FATAL_ERROR
        "DTV_USERLAND_BUILD_WINDOWS_WINSCARD requires dtv::control and dtv::host_runtime")
endif()

set(DTV_WINDOWS_WINSCARD_ADAPTER_DIR "${CMAKE_CURRENT_LIST_DIR}" CACHE INTERNAL
    "Directory of the optional Windows WinSCard adapter")
set(DTV_WINDOWS_WINSCARD_PROXY_SOURCE
    "${CMAKE_CURRENT_LIST_DIR}/windows/winscard_proxy.cpp" CACHE INTERNAL
    "Single proxy source shared by every product WinSCard DLL")

# The build-tree include directory carries only the adapter's fixed shared
# configuration. No product identity or runtime environment is compiled in.
add_library(dtv_windows_winscard_settings INTERFACE)
add_library(dtv::windows_winscard_settings ALIAS dtv_windows_winscard_settings)
target_include_directories(dtv_windows_winscard_settings INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}>)

add_library(dtv_windows_winscard_client STATIC
    "${CMAKE_CURRENT_LIST_DIR}/windows/winscard_card_client.cpp")
add_library(dtv::windows_winscard_client ALIAS dtv_windows_winscard_client)
set_target_properties(dtv_windows_winscard_client PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS OFF)
target_compile_features(dtv_windows_winscard_client PUBLIC cxx_std_17)
target_link_libraries(dtv_windows_winscard_client
    PUBLIC dtv::control
    PRIVATE dtv::core dtv::host_runtime dtv_windows_winscard_settings)

# This Core-owned target is the single WinSCard.dll artifact staged by P and A.
add_library(dtv_windows_winscard SHARED
    "${DTV_WINDOWS_WINSCARD_PROXY_SOURCE}"
    "${DTV_WINDOWS_WINSCARD_ADAPTER_DIR}/winscard.def")
add_library(dtv::windows_winscard ALIAS dtv_windows_winscard)
target_link_libraries(dtv_windows_winscard PRIVATE
    dtv_windows_winscard_client
    dtv::host_runtime
    dtv_windows_winscard_settings)
set_target_properties(dtv_windows_winscard PROPERTIES
    PREFIX ""
    OUTPUT_NAME "WinSCard"
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS OFF)
target_compile_features(dtv_windows_winscard PRIVATE cxx_std_17)

# Product builds may keep their existing target name while depending on the
# single Core artifact. Product support libraries are intentionally not linked.
function(dtv_add_windows_winscard_dll target)
    if(ARGN)
        message(FATAL_ERROR
            "The shared WinSCard DLL does not accept product-specific support targets")
    endif()
    add_custom_target(${target} DEPENDS dtv_windows_winscard)
endfunction()

if(MSVC)
    target_compile_options(dtv_windows_winscard_client PRIVATE /EHs-c- /GR- /W4)
    target_compile_options(dtv_windows_winscard PRIVATE /EHs-c- /GR- /W4)
else()
    target_compile_options(dtv_windows_winscard_client PRIVATE
        -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic)
    target_compile_options(dtv_windows_winscard PRIVATE
        -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic)
endif()
