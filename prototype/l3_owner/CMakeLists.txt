# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.16)
project(l3_owner_prototype LANGUAGES C CXX)

include(CTest)
find_package(Threads REQUIRED)

option(L3_OWNER_ENABLE_TSAN "Compile the isolated owner tests with ThreadSanitizer" OFF)
option(L3_OWNER_ENABLE_ASAN "Compile the isolated owner tests with AddressSanitizer" OFF)

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

add_executable(l3_public_contract_c tests/c_contract_regressions.c
    tests/initialize_claims_wrap.cpp)
target_include_directories(l3_public_contract_c PRIVATE include)
target_include_directories(l3_public_contract_c PRIVATE src)
target_link_libraries(l3_public_contract_c PRIVATE l3_owner_model Threads::Threads)
target_link_options(l3_public_contract_c PRIVATE
    "LINKER:--wrap=_ZN18l3_owner_prototype11OwnerEngine17initialize_claimsEj"
    "LINKER:--wrap=pthread_mutex_unlock")
target_compile_options(l3_public_contract_c PRIVATE -Wall -Wextra -Werror)
set_target_properties(l3_public_contract_c PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO
)

add_executable(l3_lease_stream_c tests/lease_stream_contract.c)
target_include_directories(l3_lease_stream_c PRIVATE include)
target_link_libraries(l3_lease_stream_c PRIVATE l3_owner_model Threads::Threads)
target_compile_options(l3_lease_stream_c PRIVATE -Wall -Wextra -Werror)
set_target_properties(l3_lease_stream_c PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO
)

add_executable(l3_waits_lanes_c tests/waits_lanes_contract.c)
target_include_directories(l3_waits_lanes_c PRIVATE include)
target_link_libraries(l3_waits_lanes_c PRIVATE l3_owner_model Threads::Threads)
target_compile_options(l3_waits_lanes_c PRIVATE -Wall -Wextra -Werror)
set_target_properties(l3_waits_lanes_c PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO
)

add_executable(l3_header_cpp tests/header_cpp.cpp)
target_include_directories(l3_header_cpp PRIVATE include)
target_compile_features(l3_header_cpp PRIVATE cxx_std_17)

if(L3_OWNER_ENABLE_TSAN)
    foreach(target l3_owner_model l3_owner_tests l3_header_c l3_public_contract_c l3_lease_stream_c l3_waits_lanes_c)
        target_compile_options(${target} PRIVATE -fsanitize=thread -fno-omit-frame-pointer -g)
        target_link_options(${target} PRIVATE -fsanitize=thread)
    endforeach()
endif()

if(L3_OWNER_ENABLE_ASAN)
    foreach(target l3_owner_model l3_owner_tests l3_header_c l3_public_contract_c l3_lease_stream_c l3_waits_lanes_c)
        target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer -g)
        target_link_options(${target} PRIVATE -fsanitize=address)
    endforeach()
endif()

if(BUILD_TESTING)
    add_test(NAME l3_owner_tests COMMAND l3_owner_tests)
    add_test(NAME l3_header_c COMMAND l3_header_c)
    add_test(NAME l3_header_cpp COMMAND l3_header_cpp)
    add_test(NAME l3_c_reject_recovery COMMAND l3_public_contract_c reject)
    add_test(NAME l3_c_open_cancel_local COMMAND l3_public_contract_c cancel)
    add_test(NAME l3_c_release_nonblocking COMMAND l3_public_contract_c release)
    add_test(NAME l3_c_open_poll_stable COMMAND l3_public_contract_c poll)
    add_test(NAME l3_c_open_cancel_preinit COMMAND l3_public_contract_c preinit-cancel)
    add_test(NAME l3_c_open_cancel_repeat COMMAND l3_public_contract_c repeat-cancel)
    add_test(NAME l3_c_open_epoch_stable COMMAND l3_public_contract_c epoch)
    add_test(NAME l3_c_open_cancel_after_setup COMMAND l3_public_contract_c after-setup)
    add_test(NAME l3_c_lease_share_exclusive COMMAND l3_lease_stream_c share)
    add_test(NAME l3_c_lease_tuning_generations COMMAND l3_lease_stream_c tuning)
    add_test(NAME l3_c_lease_ts_boundary COMMAND l3_lease_stream_c ts)
    add_test(NAME l3_c_lease_drain_eof COMMAND l3_lease_stream_c drain)
    add_test(NAME l3_c_lease_discard_unread COMMAND l3_lease_stream_c discard)
    add_test(NAME l3_c_lease_unsupported COMMAND l3_lease_stream_c unsupported)
    add_test(NAME l3_c_lease_attach_fail COMMAND l3_lease_stream_c attach-fail)
    add_test(NAME l3_c_lease_stop_residual COMMAND l3_lease_stream_c residual)
    add_test(NAME l3_c_lease_close COMMAND l3_lease_stream_c close)
    add_test(NAME l3_c_lease_attach_abort COMMAND l3_lease_stream_c attach-abort)
    add_test(NAME l3_c_wait_blocking_claim COMMAND l3_waits_lanes_c open-wait-close)
    add_test(NAME l3_c_wait_close_once COMMAND l3_waits_lanes_c close-once)
    add_test(NAME l3_c_wait_release_gate COMMAND l3_waits_lanes_c release-gate)
    add_test(NAME l3_c_wait_close_poll COMMAND l3_waits_lanes_c close-poll)
    add_test(NAME l3_c_wait_read_wait COMMAND l3_waits_lanes_c read-wait)
    add_test(NAME l3_c_wait_startup_fail COMMAND l3_waits_lanes_c startup-fail)
    add_test(NAME l3_c_wait_cancel_reentry COMMAND l3_waits_lanes_c cancel-reentry)
    add_test(NAME l3_c_wait_cleanup_reentry COMMAND l3_waits_lanes_c cleanup-reentry)
    add_test(NAME l3_c_wait_completion COMMAND l3_waits_lanes_c wait-completion)
    add_test(NAME l3_c_wait_packet COMMAND l3_waits_lanes_c wait-packet)
    add_test(NAME l3_c_claim_cancel_return COMMAND l3_waits_lanes_c claim-cancel-return)
    add_test(NAME l3_c_claim_cancel_unsupported COMMAND l3_waits_lanes_c claim-cancel-unsupported)
    add_test(NAME l3_c_claim_cancel_after_ready COMMAND l3_waits_lanes_c claim-cancel-after-ready)
endif()
