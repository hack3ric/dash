cmake_minimum_required(VERSION 3.11)
project(extracted_cceh LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-long-long -fPIC -march=native")

if(CMAKE_BUILD_TYPE STREQUAL "Release")
  message("-- Release mode, all optimizations enabled")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -g -DNDEBUG -Ofast")
endif()

add_library(cceh_extracted INTERFACE)
target_include_directories(cceh_extracted
  INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/epoch_reclaimer
)

add_executable(cceh_test_pmem tests/cceh_test_pmem.cpp)
target_link_libraries(cceh_test_pmem PRIVATE cceh_extracted pthread gflags)

add_executable(cceh_test_pmem_baseline_dram
  tests/cceh_test_pmem_baseline_dram.cpp)
target_link_libraries(cceh_test_pmem_baseline_dram PRIVATE cceh_extracted pthread gflags)

# add_executable(cceh_test_pmem_refactored tests/cceh_test_pmem_refactored.cpp)
# target_link_libraries(cceh_test_pmem_refactored PRIVATE cceh_extracted pthread)
