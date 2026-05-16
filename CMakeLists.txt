if(POLICY CMP0048)
  cmake_policy(SET CMP0048 NEW)
endif()

cmake_minimum_required(VERSION 3.11)
project(dash)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-long-long -fPIC -march=native")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(${CMAKE_BUILD_TYPE} STREQUAL "Release")
  message("-- Release mode, all optimizations enabled")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -g -DNDEBUG -Ofast")
endif()

##################### Epoch Reclaim ####################
include_directories(${CMAKE_SOURCE_DIR}/third_party/epoch_reclaimer)

set(libs_to_link pthread)

add_executable(test_pmem src/test_pmem.cpp)
add_executable(test_pmem_refactored src/test_pmem_refactored.cpp)
add_executable(example src/example.cpp)
add_executable(var_example src/var_example.cpp)
target_link_libraries(test_pmem PRIVATE ${libs_to_link} gflags)
target_link_libraries(test_pmem_refactored PRIVATE ${libs_to_link})
target_link_libraries(example PRIVATE ${libs_to_link})
target_link_libraries(var_example PRIVATE ${libs_to_link})
