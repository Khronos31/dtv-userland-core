cmake_minimum_required(VERSION 3.16)
project(dtv_userland_json_consumer LANGUAGES CXX)

find_package(dtv_userland_core REQUIRED CONFIG)

add_executable(status_json_installed_consumer main.cpp)
target_link_libraries(status_json_installed_consumer PRIVATE dtv::json)
