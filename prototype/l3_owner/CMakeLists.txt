# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.16)
project(l3_owner_prototype LANGUAGES C CXX)

include(CTest)
find_package(Threads REQUIRED)

option(L3_OWNER_ENABLE_TSAN "Compile the isolated owner tests with ThreadSanitizer" OFF)

add_library(l3_owner_model STATIC src/owner_engine.cpp src/c_api.cpp)
target_include_directories(l3_owner_model PUBLIC include PRIVATE src)
target_compile_features(l3_owner_model PUBLIC cxx_std_17)
target_link_libraries(l3_owner_model PUBLIC Threads::Threads)
set_target_properties(l3_owner_model PROPERTIES CXX_EXTENSIONS OFF)

add_executable(l3_owner_tests tests/owner_engine_tests.cpp)
target_include_directories(l3_owner_tests PRIVATE src)
target_link_libraries(l3_owner_tests PRIVATE l3_owner_model Threads::Threads)
target_compile_features(l3_owner_tests PRIVATE cxx_std_17)
set_target_properties(l3_owner_tests PROPERTIES CXX_EXTENSIONS OFF)

add_executable(l3_header_c tests/header_c.c)
target_include_directories(l3_header_c PRIVATE include)
target_link_libraries(l3_header_c PRIVATE l3_owner_model Threads::Threads)
set_target_properties(l3_header_c PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES)

add_executable(l3_header_cpp tests/header_cpp.cpp)
target_include_directories(l3_header_cpp PRIVATE include)
target_compile_features(l3_header_cpp PRIVATE cxx_std_17)

if(L3_OWNER_ENABLE_TSAN)
    foreach(target l3_owner_model l3_owner_tests l3_header_c)
        target_compile_options(${target} PRIVATE -fsanitize=thread -fno-omit-frame-pointer -g)
        target_link_options(${target} PRIVATE -fsanitize=thread)
    endforeach()
endif()

if(BUILD_TESTING)
    add_test(NAME l3_owner_tests COMMAND l3_owner_tests)
    add_test(NAME l3_header_c COMMAND l3_header_c)
    add_test(NAME l3_header_cpp COMMAND l3_header_cpp)
endif()
