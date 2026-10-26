get_filename_component(_listener_context_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if(WIN32)
    add_executable(host_listener_context_platform_tests
        windows_tests.cpp
        ${_listener_context_root}/adapters/host_listener_context/src/windows/listener_io_windows.cpp)
    target_compile_definitions(host_listener_context_platform_tests PRIVATE
        DTV_LISTENER_CONTEXT_DIAGNOSTICS=1
        DTV_LISTENER_CONTEXT_TESTING=1)
    target_include_directories(host_listener_context_platform_tests PRIVATE
        ${_listener_context_root}/adapters/host_listener_context/src
        ${_listener_context_root}/adapters/host_listener_context/include
        ${_listener_context_root}/adapters/host_runtime/include)
    target_link_libraries(host_listener_context_platform_tests PRIVATE dtv::host_runtime)
    if(MSVC)
        target_compile_options(host_listener_context_platform_tests PRIVATE /EHs-c- /GR- /W4)
        target_link_libraries(host_listener_context_platform_tests PRIVATE advapi32 ws2_32 bcrypt)
    endif()
else()
    add_executable(host_listener_context_platform_tests
        posix_tests.cpp
        ${_listener_context_root}/adapters/host_listener_context/src/posix/listener_io_posix.cpp)
    target_include_directories(host_listener_context_platform_tests PRIVATE
        ${_listener_context_root}/adapters/host_listener_context/src
        ${_listener_context_root}/adapters/host_listener_context/include
        ${_listener_context_root}/adapters/host_runtime/include)
    target_link_libraries(host_listener_context_platform_tests PRIVATE dtv::host_runtime)
    if(NOT MSVC)
        target_compile_options(host_listener_context_platform_tests PRIVATE
            -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic)
    endif()
endif()
target_compile_features(host_listener_context_platform_tests PRIVATE cxx_std_17)
add_test(NAME host_listener_context_platform_tests
    COMMAND host_listener_context_platform_tests)
