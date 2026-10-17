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

if(MSVC)
    target_compile_options(dtv_userland_core PRIVATE /EHs-c- /GR- /W4)
    target_compile_options(dtv_userland_wire PRIVATE /EHs-c- /GR- /W4)
else()
    target_compile_options(dtv_userland_core PRIVATE -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic)
    target_compile_options(dtv_userland_wire PRIVATE -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic)
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

install(TARGETS dtv_userland_core dtv_userland_wire
    EXPORT dtv_userland_coreTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
if(DTV_USERLAND_BUILD_RECEIVER)
    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
else()
    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        PATTERN "detail" EXCLUDE)
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

    if(DTV_USERLAND_BUILD_RECEIVER)
        add_executable(receiver_engine_tests tests/receiver_engine_tests.cpp)
        target_link_libraries(receiver_engine_tests PRIVATE dtv::receiver)
        add_test(NAME receiver_engine_tests COMMAND receiver_engine_tests)
        # Outer bound behind the tests' own finite waits.
        set_tests_properties(receiver_engine_tests PROPERTIES TIMEOUT 120)
    endif()

    set(DTV_LEGACY_SOURCE "" CACHE PATH "Fixed PX4 userland source tree for equivalence check")
    if(DTV_LEGACY_SOURCE)
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        add_test(NAME legacy_equivalence
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/legacy_equivalence.py
                --source ${DTV_LEGACY_SOURCE} --cxx ${CMAKE_CXX_COMPILER})
    endif()
endif()
