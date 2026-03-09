cmake_minimum_required(VERSION 3.10)
project(cpp_wal_k8s)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find threading library (required by httplib)
find_package(Threads REQUIRED)

add_executable(kv_store main.cpp)
target_link_libraries(kv_store Threads::Threads)