cmake_minimum_required(VERSION 3.16)
project(dtv_userland_core VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    include(CTest)
    set(_dtv_userland_testing_default ${BUILD_TESTING})
else()
    set(_dtv_userland_testing_default OFF)
endif()
option(DTV_USERLAND_BUILD_TESTING "Build DTV userland core tests" ${_dtv_userland_testing_default})
# The internal receiver lifecycle engine is an opt-in target so that default
# core and wire configurations keep their dependency footprint; only it
# requires Threads.
option(DTV_USERLAND_BUILD_RECEIVER "Build the internal receiver lifecycle engine" OFF)
option(DTV_USERLAND_BUILD_CARD "Build the internal card ownership engine" OFF)
option(DTV_USERLAND_BUILD_CAPTURE "Build the internal packet-session storage" OFF)
option(DTV_USERLAND_BUILD_CONTROL "Build the internal control protocol gate" OFF)
option(DTV_USERLAND_BUILD_STATUS_PROJECTION "Build the internal receiver STATUS projection" OFF)
option(DTV_USERLAND_BUILD_AUTHORITY "Build the internal control authority service" OFF)
option(DTV_USERLAND_BUILD_HOST_CTL "Build the optional GPL host control frontend" OFF)
set(DTV_USERLAND_HOST_CTL_FACADE_INCLUDE_DIRS "" CACHE STRING
    "Private host facade include directories required by the GPL host control frontend")
option(DTV_USERLAND_BUILD_HOST_RUNTIME "Build the optional GPL host runtime adapter" OFF)
option(DTV_USERLAND_BUILD_HOST_TS "Build the optional GPL host TS consumer" OFF)
option(DTV_USERLAND_BUILD_HOST_FACADE "Build the optional GPL host facade" OFF)
option(DTV_USERLAND_BUILD_HOST_DAEMON "Build the optional GPL host daemon services" OFF)
set(DTV_USERLAND_HOST_TS_FACADE_INCLUDE_DIRS "" CACHE STRING
    "Private host facade include directories required by the GPL host TS consumer")
set(DTV_USERLAND_HOST_RUNTIME_PRODUCT "" CACHE STRING
    "Product runtime namespace component required by the GPL host runtime adapter")
set(DTV_USERLAND_HOST_RUNTIME_INSTANCE_POLICY "" CACHE STRING
    "Instance-name policy for the GPL host runtime adapter: lowercase or preserve-case")
set(DTV_USERLAND_HOST_RUNTIME_FACADE_INCLUDE_DIRS "" CACHE STRING
    "Private product facade include directories required by the GPL host runtime adapter")

add_library(dtv_userland_core STATIC
    src/error.cpp
    src/logging.cpp
    src/mock_transport.cpp)
add_library(dtv::core ALIAS dtv_userland_core)
set_target_properties(dtv_userland_core PROPERTIES
    EXPORT_NAME core
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS OFF)
target_compile_features(dtv_userland_core PUBLIC cxx_std_17)
target_include_directories(dtv_userland_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

add_library(dtv_userland_wire STATIC src/wire.cpp src/records.cpp)
add_library(dtv::wire ALIAS dtv_userland_wire)
set_target_properties(dtv_userland_wire PROPERTIES
    EXPORT_NAME wire
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS OFF)
target_compile_features(dtv_userland_wire PUBLIC cxx_std_17)
target_include_directories(dtv_userland_wire PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(dtv_userland_wire PUBLIC dtv::core)

add_library(dtv_userland_identity STATIC src/identity.cpp)
add_library(dtv::identity ALIAS dtv_userland_identity)
set_target_properties(dtv_userland_identity PROPERTIES
    EXPORT_NAME identity
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS OFF)
target_compile_features(dtv_userland_identity PUBLIC cxx_std_17)
target_include_directories(dtv_userland_identity PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(dtv_userland_identity PUBLIC dtv::core)

add_library(dtv_userland_tuning STATIC src/tuning.cpp)
add_library(dtv::tuning ALIAS dtv_userland_tuning)
set_target_properties(dtv_userland_tuning PROPERTIES
    EXPORT_NAME tuning
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS OFF)
target_compile_features(dtv_userland_tuning PUBLIC cxx_std_17)
target_include_directories(dtv_userland_tuning PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(dtv_userland_tuning PUBLIC dtv::wire)

add_library(dtv_userland_json STATIC src/json.cpp src/discovery_json.cpp)
add_library(dtv::json ALIAS dtv_userland_json)
set_target_properties(dtv_userland_json PROPERTIES
    EXPORT_NAME json
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS OFF)
target_compile_features(dtv_userland_json PUBLIC cxx_std_17)
target_include_directories(dtv_userland_json PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(dtv_userland_json PUBLIC dtv::wire)

if(DTV_USERLAND_BUILD_CONTROL)
    add_library(dtv_userland_control STATIC src/control_connection.cpp src/control_client.cpp)
    add_library(dtv::control ALIAS dtv_userland_control)
    set_target_properties(dtv_userland_control PROPERTIES
        EXPORT_NAME control
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS OFF)
    target_compile_features(dtv_userland_control PUBLIC cxx_std_17)
    target_include_directories(dtv_userland_control PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_link_libraries(dtv_userland_control PUBLIC dtv::wire)
endif()

if(MSVC)
    target_compile_options(dtv_userland_core PRIVATE /EHs-c- /GR- /W4)
    target_compile_options(dtv_userland_wire PRIVATE /EHs-c- /GR- /W4)
    target_compile_options(dtv_userland_identity PRIVATE /EHs-c- /GR- /W4)
    target_compile_options(dtv_userland_tuning PRIVATE /EHs-c- /GR- /W4)
    target_compile_options(dtv_userland_json PRIVATE /EHs-c- /GR- /W4)
    if(DTV_USERLAND_BUILD_CONTROL)
        target_compile_options(dtv_userland_control PRIVATE /EHs-c- /GR- /W4)
    endif()
else()
    target_compile_options(dtv_userland_core PRIVATE -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic)
    target_compile_options(dtv_userland_wire PRIVATE -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic)
    target_compile_options(dtv_userland_identity PRIVATE -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic)
    target_compile_options(dtv_userland_tuning PRIVATE -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic)
    target_compile_options(dtv_userland_json PRIVATE -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic)
    if(DTV_USERLAND_BUILD_CONTROL)
        target_compile_options(dtv_userland_control PRIVATE
            -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic)
    endif()
endif()

if(DTV_USERLAND_BUILD_RECEIVER)
    find_package(Threads REQUIRED)
    add_library(dtv_userland_receiver STATIC src/receiver_engine.cpp)
    add_library(dtv::receiver ALIAS dtv_userland_receiver)
    set_target_properties(dtv_userland_receiver PROPERTIES
        EXPORT_NAME receiver
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS OFF)
    target_compile_features(dtv_userland_receiver PUBLIC cxx_std_17)
    target_include_directories(dtv_userland_receiver PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_link_libraries(dtv_userland_receiver PUBLIC dtv::core Threads::Threads)
    if(MSVC)
        target_compile_options(dtv_userland_receiver PRIVATE /EHs-c- /GR- /W4)
    else()
        target_compile_options(dtv_userland_receiver PRIVATE
            -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic)
    endif()
endif()
if(DTV_USERLAND_BUILD_STATUS_PROJECTION)
    if(NOT DTV_USERLAND_BUILD_CONTROL OR NOT DTV_USERLAND_BUILD_RECEIVER)
        message(FATAL_ERROR
            "DTV_USERLAND_BUILD_STATUS_PROJECTION requires DTV_USERLAND_BUILD_CONTROL and DTV_USERLAND_BUILD_RECEIVER")
    endif()
    add_library(dtv_userland_status_projection STATIC src/status_projection.cpp)
    add_library(dtv::status_projection ALIAS dtv_userland_status_projection)
    set_target_properties(dtv_userland_status_projection PROPERTIES
        EXPORT_NAME status_projection
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS OFF)
    target_compile_features(dtv_userland_status_projection PUBLIC cxx_std_17)
    target_include_directories(dtv_userland_status_projection PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_link_libraries(dtv_userland_status_projection PUBLIC dtv::control dtv::receiver)
    if(MSVC)
        target_compile_options(dtv_userland_status_projection PRIVATE /EHs-c- /GR- /W4)
    else()
        target_compile_options(dtv_userland_status_projection PRIVATE
            -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic)
    endif()
endif()
if(DTV_USERLAND_BUILD_AUTHORITY)
    if(NOT DTV_USERLAND_BUILD_STATUS_PROJECTION)
        message(FATAL_ERROR
            "DTV_USERLAND_BUILD_AUTHORITY requires DTV_USERLAND_BUILD_STATUS_PROJECTION")
    endif()
    add_library(dtv_userland_authority STATIC src/control_authority.cpp)
    add_library(dtv::authority ALIAS dtv_userland_authority)
    set_target_properties(dtv_userland_authority PROPERTIES
        EXPORT_NAME authority
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS OFF)
    target_compile_features(dtv_userland_authority PUBLIC cxx_std_17)
    target_include_directories(dtv_userland_authority PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_link_libraries(dtv_userland_authority PUBLIC dtv::status_projection)
    if(MSVC)
        target_compile_options(dtv_userland_authority PRIVATE /EHs-c- /GR- /W4)
    else()
        target_compile_options(dtv_userland_authority PRIVATE
            -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic)
    endif()
endif()
if(DTV_USERLAND_BUILD_STATUS_PROJECTION)
    install(TARGETS dtv_userland_status_projection
        EXPORT dtv_userland_status_projectionTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(EXPORT dtv_userland_status_projectionTargets
        FILE dtv_userland_status_projectionTargets.cmake
        NAMESPACE dtv::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dtv_userland_core)
    install(FILES include/dtv/userland/detail/status_projection.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dtv/userland/detail)
endif()
if(DTV_USERLAND_BUILD_AUTHORITY)
    install(TARGETS dtv_userland_authority
        EXPORT dtv_userland_authorityTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(EXPORT dtv_userland_authorityTargets
        FILE dtv_userland_authorityTargets.cmake
        NAMESPACE dtv::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dtv_userland_core)
    install(FILES include/dtv/userland/detail/control_authority.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dtv/userland/detail)
endif()

if(DTV_USERLAND_BUILD_CAPTURE)
    find_package(Threads REQUIRED)
    add_library(dtv_userland_capture STATIC src/packet_stream_session.cpp)
    add_library(dtv::capture ALIAS dtv_userland_capture)
    set_target_properties(dtv_userland_capture PROPERTIES
        EXPORT_NAME capture
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS OFF)
    target_compile_features(dtv_userland_capture PUBLIC cxx_std_17)
    target_include_directories(dtv_userland_capture PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_link_libraries(dtv_userland_capture PUBLIC dtv::core Threads::Threads)
    if(MSVC)
        target_compile_options(dtv_userland_capture PRIVATE /EHs-c- /GR- /W4)
    else()
        target_compile_options(dtv_userland_capture PRIVATE
            -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic)
    endif()
endif()

if(DTV_USERLAND_BUILD_CARD)
    add_library(dtv_userland_card INTERFACE)
    add_library(dtv::card ALIAS dtv_userland_card)
    set_target_properties(dtv_userland_card PROPERTIES EXPORT_NAME card)
    target_compile_features(dtv_userland_card INTERFACE cxx_std_17)
    target_include_directories(dtv_userland_card INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_link_libraries(dtv_userland_card INTERFACE dtv::core)
endif()

if(DTV_USERLAND_BUILD_HOST_CTL)
    if(NOT DTV_USERLAND_HOST_CTL_FACADE_INCLUDE_DIRS)
        message(FATAL_ERROR
            "DTV_USERLAND_HOST_CTL_FACADE_INCLUDE_DIRS must name the product host facade headers")
    endif()
    add_library(dtv_userland_host_ctl STATIC adapters/host_ctl/host_ctl.cpp)
    add_library(dtv::host_ctl ALIAS dtv_userland_host_ctl)
    set_target_properties(dtv_userland_host_ctl PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS OFF)
    target_compile_features(dtv_userland_host_ctl PUBLIC cxx_std_17)
    target_include_directories(dtv_userland_host_ctl
        PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/adapters/host_ctl>
        PRIVATE ${DTV_USERLAND_HOST_CTL_FACADE_INCLUDE_DIRS}
                ${CMAKE_CURRENT_SOURCE_DIR}/adapters/host_facade/include
                ${CMAKE_CURRENT_SOURCE_DIR}/adapters/host_runtime/include)
    target_link_libraries(dtv_userland_host_ctl PUBLIC
        dtv::control dtv::identity dtv::json)
endif()

if(DTV_USERLAND_BUILD_HOST_RUNTIME)
    if(NOT UNIX AND NOT WIN32)
        message(FATAL_ERROR "DTV_USERLAND_BUILD_HOST_RUNTIME requires a POSIX or Windows target")
    endif()
    if(NOT DTV_USERLAND_HOST_RUNTIME_PRODUCT MATCHES "^[a-z0-9]+(-[a-z0-9]+)*-userland$")
        message(FATAL_ERROR
            "DTV_USERLAND_HOST_RUNTIME_PRODUCT must be an explicit product-userland namespace")
    endif()
    if(NOT DTV_USERLAND_HOST_RUNTIME_INSTANCE_POLICY STREQUAL "lowercase" AND
       NOT DTV_USERLAND_HOST_RUNTIME_INSTANCE_POLICY STREQUAL "preserve-case")
        message(FATAL_ERROR
            "DTV_USERLAND_HOST_RUNTIME_INSTANCE_POLICY must be lowercase or preserve-case")
    endif()
    if(NOT DTV_USERLAND_HOST_RUNTIME_FACADE_INCLUDE_DIRS)
        message(FATAL_ERROR
            "DTV_USERLAND_HOST_RUNTIME_FACADE_INCLUDE_DIRS must name the product facade headers")
    endif()

    if(WIN32)
        set(_host_runtime_sources
            adapters/host_runtime/windows/windows_ipc.cpp
            adapters/host_runtime/windows/windows_tuner_nonce.cpp)
    else()
        set(_host_runtime_sources
            adapters/host_runtime/posix_ipc.cpp
            adapters/host_runtime/posix_tuner_nonce.cpp)
    endif()
    add_library(dtv_userland_host_runtime STATIC ${_host_runtime_sources})
    add_library(dtv::host_runtime ALIAS dtv_userland_host_runtime)
    set_target_properties(dtv_userland_host_runtime PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS OFF)
    target_compile_features(dtv_userland_host_runtime PUBLIC cxx_std_17)
    target_include_directories(dtv_userland_host_runtime
        PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/adapters/host_runtime/include>
        PRIVATE ${DTV_USERLAND_HOST_RUNTIME_FACADE_INCLUDE_DIRS}
                ${CMAKE_CURRENT_SOURCE_DIR}/adapters/host_facade/include
                ${CMAKE_CURRENT_SOURCE_DIR}/adapters/host_daemon/include)
    target_compile_definitions(dtv_userland_host_runtime PRIVATE
        PX4_USERLAND_RUNTIME_PRODUCT="${DTV_USERLAND_HOST_RUNTIME_PRODUCT}")
    if(DTV_USERLAND_HOST_RUNTIME_INSTANCE_POLICY STREQUAL "preserve-case")
        target_compile_definitions(dtv_userland_host_runtime PRIVATE
            PX4_USERLAND_RUNTIME_INSTANCE_PRESERVE_CASE=1)
    else()
        target_compile_definitions(dtv_userland_host_runtime PRIVATE
            PX4_USERLAND_RUNTIME_INSTANCE_LOWERCASE=1)
    endif()
    target_link_libraries(dtv_userland_host_runtime PUBLIC dtv::identity)
    if(WIN32)
        target_link_libraries(dtv_userland_host_runtime PUBLIC advapi32 bcrypt ws2_32)
    endif()
endif()

if(DTV_USERLAND_BUILD_HOST_TS)
    if(NOT TARGET dtv::host_runtime)
        message(FATAL_ERROR
            "DTV_USERLAND_BUILD_HOST_TS requires DTV_USERLAND_BUILD_HOST_RUNTIME")
    endif()
    if(NOT DTV_USERLAND_HOST_TS_FACADE_INCLUDE_DIRS)
        message(FATAL_ERROR
            "DTV_USERLAND_HOST_TS_FACADE_INCLUDE_DIRS must name the product host facade headers")
    endif()
    set(_host_ts_sources
        adapters/host_ts/control_client.cpp
        adapters/host_ts/px4_ts_core.cpp)
    if(WIN32)
        list(APPEND _host_ts_sources adapters/host_ts/windows/px4_ts_windows_output.cpp)
    elseif(UNIX)
        list(APPEND _host_ts_sources adapters/host_ts/posix/px4_ts_posix.cpp)
    else()
        message(FATAL_ERROR "DTV_USERLAND_BUILD_HOST_TS requires a POSIX or Windows target")
    endif()
    add_library(dtv_userland_host_ts STATIC ${_host_ts_sources})
    add_library(dtv::host_ts ALIAS dtv_userland_host_ts)
    set_target_properties(dtv_userland_host_ts PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS OFF)
    target_compile_features(dtv_userland_host_ts PUBLIC cxx_std_17)
    target_include_directories(dtv_userland_host_ts
        PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/adapters/host_ts/include>
        PRIVATE ${DTV_USERLAND_HOST_TS_FACADE_INCLUDE_DIRS}
                ${CMAKE_CURRENT_SOURCE_DIR}/adapters/host_facade/include
                ${CMAKE_CURRENT_SOURCE_DIR}/adapters/host_runtime/include)
    target_link_libraries(dtv_userland_host_ts PUBLIC
        dtv::control dtv::identity dtv::tuning dtv::host_runtime)
    if(MSVC)
        target_compile_options(dtv_userland_host_ts PRIVATE /EHs-c- /GR- /W4)
    else()
        target_compile_options(dtv_userland_host_ts PRIVATE
            -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic)
    endif()
endif()

if(DTV_USERLAND_BUILD_HOST_FACADE OR DTV_USERLAND_BUILD_HOST_DAEMON)
    add_library(dtv_userland_host_facade STATIC
        adapters/host_facade/src/ipc.cpp
        adapters/host_facade/src/identity.cpp
        adapters/host_facade/src/px4d_signals.cpp)
    add_library(dtv::host_facade ALIAS dtv_userland_host_facade)
    set_target_properties(dtv_userland_host_facade PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS OFF)
    target_compile_features(dtv_userland_host_facade PUBLIC cxx_std_17)
    target_include_directories(dtv_userland_host_facade
        PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/adapters/host_facade/include>
               $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/adapters/host_runtime/include>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/adapters/host_facade/src)
    target_link_libraries(dtv_userland_host_facade PUBLIC dtv::core dtv::identity)
    if(WIN32)
        target_sources(dtv_userland_host_facade PRIVATE
            adapters/host_facade/src/windows_sleep.cpp
            adapters/host_facade/src/px4d_windows_stdin.cpp)
        target_link_libraries(dtv_userland_host_facade PUBLIC advapi32)
    endif()
    if(MSVC)
        target_compile_options(dtv_userland_host_facade PRIVATE /EHs-c- /GR- /W4)
    else()
        target_compile_options(dtv_userland_host_facade PRIVATE
            -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic)
    endif()
endif()

if(DTV_USERLAND_BUILD_HOST_DAEMON)
    if(NOT TARGET dtv::host_facade OR NOT TARGET dtv::host_runtime OR
       NOT TARGET dtv::host_ts)
        message(FATAL_ERROR
            "DTV_USERLAND_BUILD_HOST_DAEMON requires host facade, runtime and TS")
    endif()
    find_package(Threads REQUIRED)
    add_library(dtv_userland_host_daemon STATIC
        adapters/host_daemon/src/windows_pthread_compat.h
        adapters/host_daemon/src/card_service.cpp
        adapters/host_daemon/src/control_server.cpp
        adapters/host_daemon/src/control_workers.cpp
        adapters/host_daemon/src/enclosure_context.cpp
        adapters/host_daemon/src/tuner_service.cpp)
    add_library(dtv::host_daemon ALIAS dtv_userland_host_daemon)
    set_target_properties(dtv_userland_host_daemon PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS OFF)
    target_compile_features(dtv_userland_host_daemon PUBLIC cxx_std_17)
    target_include_directories(dtv_userland_host_daemon
        PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/adapters/host_daemon/include>
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/adapters/host_daemon/src
                ${CMAKE_CURRENT_SOURCE_DIR}/adapters/host_facade/include
                ${CMAKE_CURRENT_SOURCE_DIR}/adapters/host_runtime/include)
    target_link_libraries(dtv_userland_host_daemon PUBLIC
        Threads::Threads dtv::host_facade dtv::host_runtime dtv::host_ts
        dtv::receiver dtv::card dtv::capture dtv::control dtv::identity)
    if(MSVC)
        target_compile_options(dtv_userland_host_daemon PRIVATE /EHs-c- /GR- /W4)
    else()
        target_compile_options(dtv_userland_host_daemon PRIVATE
            -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic)
    endif()
endif()

if(TARGET dtv::host_runtime AND TARGET dtv::host_facade)
    target_link_libraries(dtv_userland_host_runtime PUBLIC dtv::host_facade)
endif()

install(TARGETS dtv_userland_core dtv_userland_wire dtv_userland_identity dtv_userland_tuning
    dtv_userland_json
    EXPORT dtv_userland_coreTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
if(DTV_USERLAND_BUILD_CARD)
    install(TARGETS dtv_userland_card EXPORT dtv_userland_coreTargets)
endif()
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    PATTERN "detail" EXCLUDE
    PATTERN "control_connection.h" EXCLUDE
    PATTERN "control_client.h" EXCLUDE)
install(FILES
    include/dtv/userland/detail/text.h
    include/dtv/userland/detail/sha256.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dtv/userland/detail)
if(DTV_USERLAND_BUILD_RECEIVER OR DTV_USERLAND_BUILD_CAPTURE)
    install(FILES include/dtv/userland/detail/stream_session_types.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dtv/userland/detail)
endif()
if(DTV_USERLAND_BUILD_RECEIVER)
    install(FILES include/dtv/userland/detail/receiver_engine.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dtv/userland/detail)
endif()
if(DTV_USERLAND_BUILD_CAPTURE)
    install(FILES include/dtv/userland/detail/packet_stream_session.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dtv/userland/detail)
endif()
if(DTV_USERLAND_BUILD_CONTROL)
    install(TARGETS dtv_userland_control
        EXPORT dtv_userland_controlTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(EXPORT dtv_userland_controlTargets
        FILE dtv_userland_controlTargets.cmake
        NAMESPACE dtv::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dtv_userland_core)
endif()
if(DTV_USERLAND_BUILD_CONTROL)
    install(FILES
        include/dtv/userland/control_connection.h
        include/dtv/userland/control_client.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dtv/userland)
endif()
if(DTV_USERLAND_BUILD_CARD)
    install(FILES
        include/dtv/userland/detail/card_ownership_engine.h
        include/dtv/userland/detail/card_ownership_engine.tpp
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dtv/userland/detail)
endif()
install(EXPORT dtv_userland_coreTargets
    FILE dtv_userland_coreTargets.cmake
    NAMESPACE dtv::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dtv_userland_core)
if(DTV_USERLAND_BUILD_RECEIVER)
    # Loaded only for find_package(... COMPONENTS receiver), which also finds
    # Threads for the consumer.
    install(TARGETS dtv_userland_receiver
        EXPORT dtv_userland_receiverTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(EXPORT dtv_userland_receiverTargets
        FILE dtv_userland_receiverTargets.cmake
        NAMESPACE dtv::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dtv_userland_core)
endif()
if(DTV_USERLAND_BUILD_CAPTURE)
    install(TARGETS dtv_userland_capture
        EXPORT dtv_userland_captureTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(EXPORT dtv_userland_captureTargets
        FILE dtv_userland_captureTargets.cmake
        NAMESPACE dtv::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dtv_userland_core)
endif()
include(CMakePackageConfigHelpers)
configure_package_config_file(cmake/dtv_userland_coreConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/dtv_userland_coreConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dtv_userland_core)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/dtv_userland_coreConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/dtv_userland_coreConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/dtv_userland_coreConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dtv_userland_core)
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/dtv_userland_core)

if(DTV_USERLAND_BUILD_TESTING)
    add_executable(foundation_tests tests/foundation_tests.cpp)
    target_link_libraries(foundation_tests PRIVATE dtv::core)
    add_test(NAME foundation_tests COMMAND foundation_tests)

    add_executable(wire_tests tests/wire_tests.cpp)
    target_link_libraries(wire_tests PRIVATE dtv::wire)
    add_test(NAME wire_tests COMMAND wire_tests)

    add_executable(records_tests tests/records_tests.cpp)
    target_link_libraries(records_tests PRIVATE dtv::wire)
    add_test(NAME records_tests COMMAND records_tests)

    add_executable(identity_tests tests/identity_tests.cpp)
    target_link_libraries(identity_tests PRIVATE dtv::identity)
    add_test(NAME identity_tests COMMAND identity_tests)

    add_executable(tuning_tests tests/tuning_tests.cpp)
    target_link_libraries(tuning_tests PRIVATE dtv::tuning)
    add_test(NAME tuning_tests COMMAND tuning_tests)

    if(DTV_USERLAND_BUILD_CONTROL)
        add_executable(control_connection_tests tests/control_connection_tests.cpp)
        target_link_libraries(control_connection_tests PRIVATE dtv::control)
        add_test(NAME control_connection_tests COMMAND control_connection_tests)

        add_executable(control_client_tests tests/control_client_tests.cpp)
        target_link_libraries(control_client_tests PRIVATE dtv::control)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
           CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
            find_package(Threads REQUIRED)
            target_link_libraries(control_client_tests PRIVATE Threads::Threads)
            target_compile_definitions(control_client_tests PRIVATE
                DTV_USERLAND_CONTROL_STACK_PROBES=1)
        endif()
        add_test(NAME control_client_tests COMMAND control_client_tests)
    endif()

    add_executable(status_json_tests tests/status_json_tests.cpp)
    target_link_libraries(status_json_tests PRIVATE dtv::json)
    add_test(NAME status_json_tests COMMAND status_json_tests)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME status_json_consumer
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/status_json_consumer.py
            $<TARGET_FILE:status_json_tests>)

    add_executable(discovery_json_tests tests/discovery_json_tests.cpp)
    target_link_libraries(discovery_json_tests PRIVATE dtv::json)
    add_test(NAME discovery_json_tests COMMAND discovery_json_tests)
    add_test(NAME discovery_json_consumer
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/discovery_json_consumer.py
            $<TARGET_FILE:discovery_json_tests>)

    if(DTV_USERLAND_BUILD_CARD)
        add_executable(card_ownership_engine_tests tests/card_ownership_engine_tests.cpp)
        target_link_libraries(card_ownership_engine_tests PRIVATE dtv::card)
        if(MSVC)
            target_compile_options(card_ownership_engine_tests PRIVATE /EHs-c- /GR- /W4)
        else()
            target_compile_options(card_ownership_engine_tests PRIVATE
                -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic)
        endif()
        add_test(NAME card_ownership_engine_tests COMMAND card_ownership_engine_tests)
        set_tests_properties(card_ownership_engine_tests PROPERTIES TIMEOUT 120)
    endif()

    if(DTV_USERLAND_BUILD_RECEIVER)
        add_executable(receiver_engine_tests tests/receiver_engine_tests.cpp)
        target_link_libraries(receiver_engine_tests PRIVATE dtv::receiver)
        add_test(NAME receiver_engine_tests COMMAND receiver_engine_tests)
        # Outer bound behind the tests' own finite waits.
        set_tests_properties(receiver_engine_tests PROPERTIES TIMEOUT 120)

        add_executable(receiver_r2_tests tests/receiver_r2_tests.cpp)
        target_link_libraries(receiver_r2_tests PRIVATE dtv::receiver)
        # Test-binary-only real-mutex observer for the registry rendezvous
        # proofs.  Linux GNU/Clang only; other platforms compile it out and the
        # portable tests remain unchanged.
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux"
           AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_definitions(receiver_r2_tests PRIVATE DTV_R2_LOCK_OBSERVER=1)
            target_link_options(receiver_r2_tests PRIVATE
                "LINKER:--wrap=pthread_mutex_lock")
        endif()
        add_test(NAME receiver_r2_tests COMMAND receiver_r2_tests)
        set_tests_properties(receiver_r2_tests PROPERTIES TIMEOUT 120)

        add_executable(receiver_hz_tests tests/receiver_hz_tests.cpp)
        target_link_libraries(receiver_hz_tests PRIVATE dtv::receiver)
        add_test(NAME receiver_hz_tests COMMAND receiver_hz_tests)
        set_tests_properties(receiver_hz_tests PROPERTIES TIMEOUT 120)
    endif()

    if(DTV_USERLAND_BUILD_STATUS_PROJECTION)
        add_executable(status_projection_tests tests/status_projection_tests.cpp)
        target_link_libraries(status_projection_tests PRIVATE dtv::status_projection)
        add_test(NAME status_projection_tests COMMAND status_projection_tests)
    endif()

    if(DTV_USERLAND_BUILD_AUTHORITY)
        add_executable(control_authority_tests tests/control_authority_tests.cpp)
        target_link_libraries(control_authority_tests PRIVATE dtv::authority)
        add_test(NAME control_authority_tests COMMAND control_authority_tests)
        set_tests_properties(control_authority_tests PROPERTIES TIMEOUT 120)
    endif()

    if(DTV_USERLAND_BUILD_CAPTURE)
        add_executable(packet_stream_session_tests tests/packet_stream_session_tests.cpp)
        target_link_libraries(packet_stream_session_tests PRIVATE dtv::capture)
        add_test(NAME packet_stream_session_tests COMMAND packet_stream_session_tests)
        set_tests_properties(packet_stream_session_tests PROPERTIES TIMEOUT 120)
    endif()

    set(DTV_LEGACY_SOURCE "" CACHE PATH "Fixed PX4 userland source tree for equivalence check")
    if(DTV_LEGACY_SOURCE)
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        add_test(NAME legacy_equivalence
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/legacy_equivalence.py
                --source ${DTV_LEGACY_SOURCE} --cxx ${CMAKE_CXX_COMPILER})
    endif()
endif()
