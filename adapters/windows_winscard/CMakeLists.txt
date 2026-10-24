# SPDX-License-Identifier: GPL-2.0-only
#
# Optional, build-tree-only GPL-2.0-only Windows WinSCard adapter.  It is never
# installed, exported, or linked by a normal MIT Core target.  A product that
# enables it supplies its own identity and host facade includes below.

if(NOT WIN32)
    message(FATAL_ERROR
        "DTV_USERLAND_BUILD_WINDOWS_WINSCARD requires a Windows target")
endif()

foreach(_dtv_winscard_setting
        DTV_WINDOWS_WINSCARD_NAMESPACE_COMPONENT
        DTV_WINDOWS_WINSCARD_VERSION_COMPONENT
        DTV_WINDOWS_WINSCARD_PRODUCT_SLUG
        DTV_WINDOWS_WINSCARD_READER_PREFIX
        DTV_WINDOWS_WINSCARD_RUNTIME_ENV
        DTV_WINDOWS_WINSCARD_HOST_INCLUDE_DIRS)
    if(NOT DEFINED ${_dtv_winscard_setting} OR
            "${${_dtv_winscard_setting}}" STREQUAL "")
        message(FATAL_ERROR
            "${_dtv_winscard_setting} must be set by the product that enables "
            "DTV_USERLAND_BUILD_WINDOWS_WINSCARD; there is no implicit default")
    endif()
endforeach()

set(DTV_WINDOWS_WINSCARD_ADAPTER_DIR "${CMAKE_CURRENT_LIST_DIR}" CACHE INTERNAL
    "Directory of the optional Windows WinSCard adapter")
set(DTV_WINDOWS_WINSCARD_PROXY_SOURCE
    "${CMAKE_CURRENT_LIST_DIR}/windows/winscard_proxy.cpp" CACHE INTERNAL
    "Single proxy source shared by every product WinSCard DLL")

configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/windows/winscard_product.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/windows/winscard_product.h"
    @ONLY)

# The build-tree include directory carries only the generated product header;
# the adapter directory carries the checked-in sources.  Consumers that compile
# the proxy for a test get the same values without duplicating settings.
add_library(dtv_windows_winscard_settings INTERFACE)
add_library(dtv::windows_winscard_settings ALIAS dtv_windows_winscard_settings)
target_include_directories(dtv_windows_winscard_settings INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>)

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
    PRIVATE dtv::core dtv_windows_winscard_settings)
target_include_directories(dtv_windows_winscard_client PRIVATE
    ${DTV_WINDOWS_WINSCARD_HOST_INCLUDE_DIRS})
if(TARGET dtv::host_runtime)
    target_link_libraries(dtv_windows_winscard_client PRIVATE dtv::host_runtime)
endif()

# Products call this after adding the Core subdirectory.  SUPPORT names their
# client-only target that supplies the product's existing IPC/facade symbols;
# the Core side never links back to a product target.
function(dtv_add_windows_winscard_dll target)
    cmake_parse_arguments(ARG "" "" "SUPPORT" ${ARGN})
    add_library(${target} SHARED
        "${DTV_WINDOWS_WINSCARD_PROXY_SOURCE}"
        "${DTV_WINDOWS_WINSCARD_ADAPTER_DIR}/winscard.def")
    target_link_libraries(${target} PRIVATE
        dtv_windows_winscard_client
        dtv_windows_winscard_settings
        ${ARG_SUPPORT})
    target_include_directories(${target} PRIVATE
        ${DTV_WINDOWS_WINSCARD_HOST_INCLUDE_DIRS})
    set_target_properties(${target} PROPERTIES
        PREFIX ""
        OUTPUT_NAME "WinSCard"
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS OFF)
    target_compile_features(${target} PRIVATE cxx_std_17)
    if(MSVC)
        target_compile_options(${target} PRIVATE /EHs-c- /GR- /W4)
        target_compile_options(dtv_windows_winscard_client PRIVATE /EHs-c- /GR- /W4)
    else()
        target_compile_options(${target} PRIVATE
            -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic)
        target_compile_options(dtv_windows_winscard_client PRIVATE
            -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic)
    endif()
endfunction()
