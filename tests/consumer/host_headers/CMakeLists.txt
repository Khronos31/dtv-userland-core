cmake_minimum_required(VERSION 3.16)
project(dtv_userland_host_headers_consumer LANGUAGES CXX)

enable_testing()

# These probes must be satisfied by the installed package alone.  Refuse a
# configure whose imported include directories could come from the Core source
# tree instead of CMAKE_PREFIX_PATH.
if(NOT CMAKE_PREFIX_PATH)
    message(FATAL_ERROR "host header probes require CMAKE_PREFIX_PATH=<Core install prefix>")
endif()

find_package(dtv_userland_core CONFIG REQUIRED COMPONENTS host_runtime)

get_filename_component(_core_source_root "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(_source_include_roots
    "${_core_source_root}/include"
    "${_core_source_root}/adapters/host_daemon/include"
    "${_core_source_root}/adapters/host_runtime/include"
    "${_core_source_root}/adapters/host_facade/include"
    "${_core_source_root}/adapters/host_ts/include"
    "${_core_source_root}/adapters/host_ctl/include")
foreach(_target dtv::host_runtime dtv::host_tuner dtv::host_workers)
    get_target_property(_include_dirs ${_target} INTERFACE_INCLUDE_DIRECTORIES)
    foreach(_include_dir IN LISTS _include_dirs)
        foreach(_source_include_root IN LISTS _source_include_roots)
            string(FIND "${_include_dir}" "${_source_include_root}" _source_root_at)
            if(_source_root_at EQUAL 0)
                message(FATAL_ERROR
                    "${_target} resolves an include to the Core source tree: ${_include_dir}")
            endif()
        endforeach()
        set(_installed NO)
        foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
            get_filename_component(_absolute_prefix "${_prefix}" ABSOLUTE)
            string(FIND "${_include_dir}" "${_absolute_prefix}" _prefix_at)
            if(_prefix_at EQUAL 0)
                set(_installed YES)
            endif()
        endforeach()
        if(NOT _installed)
            message(FATAL_ERROR
                "${_target} include is outside CMAKE_PREFIX_PATH: ${_include_dir}")
        endif()
    endforeach()
endforeach()

foreach(_probe
        tuner_service_canonical
        tuner_service_forwarder
        card_service_canonical
        card_service_forwarder
        posix_ipc_canonical
        posix_ipc_forwarder)
    add_executable(${_probe} ${_probe}.cpp)
    target_compile_features(${_probe} PRIVATE cxx_std_17)
    target_link_libraries(${_probe} PRIVATE dtv::host_runtime)
    add_test(NAME ${_probe} COMMAND ${_probe})
endforeach()
