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

add_library(dtv_userland_wire STATIC src/wire.cpp)
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

install(TARGETS dtv_userland_core dtv_userland_wire
    EXPORT dtv_userland_coreTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT dtv_userland_coreTargets
    FILE dtv_userland_coreTargets.cmake
    NAMESPACE dtv::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dtv_userland_core)
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

    set(DTV_LEGACY_SOURCE "" CACHE PATH "Fixed PX4 userland source tree for equivalence check")
    if(DTV_LEGACY_SOURCE)
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        add_test(NAME legacy_equivalence
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/legacy_equivalence.py
                --source ${DTV_LEGACY_SOURCE} --cxx ${CMAKE_CXX_COMPILER})
    endif()
endif()
