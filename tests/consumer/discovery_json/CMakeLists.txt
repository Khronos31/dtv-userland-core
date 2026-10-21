cmake_minimum_required(VERSION 3.16)
project(dtv_userland_discovery_json_consumer LANGUAGES CXX)

find_package(dtv_userland_core REQUIRED CONFIG)

add_executable(discovery_json_installed_consumer main.cpp)
target_link_libraries(discovery_json_installed_consumer PRIVATE dtv::json)
