cmake_minimum_required(VERSION 3.16)
project(asf_mutex LANGUAGES CXX)

add_library(queue_mutex INTERFACE)
add_library(asf::queue_mutex ALIAS queue_mutex)
target_compile_features(queue_mutex INTERFACE cxx_std_17)
target_include_directories(queue_mutex INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

find_package(Threads REQUIRED)

function(asf_mutex_binary target)
  target_link_libraries(${target} PRIVATE queue_mutex Threads::Threads)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target} PRIVATE -Ofast -Wall -Wextra)
  elseif(MSVC)
    target_compile_options(${target} PRIVATE /W4 /O2)
  endif()
  if(MINGW)
    target_link_options(${target} PRIVATE -static)
  endif()
endfunction()

add_executable(test_queue_mutex tests/test_queue_mutex.cpp)
target_compile_definitions(test_queue_mutex PRIVATE ASF_TRY_SPIN=0)
asf_mutex_binary(test_queue_mutex)

add_executable(bench_mutex bench/bench_mutex.cpp)
asf_mutex_binary(bench_mutex)

enable_testing()
add_test(NAME queue_mutex COMMAND test_queue_mutex)
# The churn loop runs for 60 seconds. Leave room for a slow runner.
set_tests_properties(queue_mutex PROPERTIES TIMEOUT 180)
