# adapted a bit from http://llvm.org/docs/CMake.html#embedding-llvm-in-your-project

cmake_minimum_required(VERSION 3.2)
project(IntervalBitwise)

find_package(LLVM REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

set(CMAKE_CXX_FLAGS "-std=c++11 -fno-rtti -Wall")

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

add_executable(opt-fuzz opt-fuzz.cpp)

llvm_map_components_to_libnames(llvm_libs support core irreader)

target_link_libraries(opt-fuzz ${llvm_libs})
