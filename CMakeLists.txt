cmake_minimum_required(VERSION 2.6)

find_package(Boost)
include_directories(${Boost_INCLUDE_DIRS})

add_definitions(-Wall -Wextra)

add_executable(rekri rekri.cpp)
