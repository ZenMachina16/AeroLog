cmake_minimum_required(VERSION 3.16)
project(AeroLog VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Release build flags ─────────────────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_compile_options(
    -Wall -Wextra -Wpedantic
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:Release>:-march=native>
    $<$<CONFIG:Debug>:-g>
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>
)
add_link_options(
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>
)

# ── Optional: CPU affinity pinning ─────────────────────────────────────────
option(ENABLE_CPU_AFFINITY
    "Pin producer/consumer to dedicated CPU cores via sched_setaffinity" OFF)

# ── Helper to apply common settings ────────────────────────────────────────
function(aerolog_target name)
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(${name} PRIVATE rt)
    if(ENABLE_CPU_AFFINITY)
        target_compile_definitions(${name} PRIVATE ENABLE_CPU_AFFINITY)
    endif()
endfunction()

# ── Executables ─────────────────────────────────────────────────────────────
add_executable(producer   src/producer.cpp)
aerolog_target(producer)

add_executable(consumer   src/consumer.cpp)
aerolog_target(consumer)

add_executable(latency_test benchmarks/latency_test.cpp)
aerolog_target(latency_test)
target_link_libraries(latency_test PRIVATE pthread)

# ── Tests ───────────────────────────────────────────────────────────────────
enable_testing()

add_executable(ring_test tests/ring_buffer_test.cpp)
target_include_directories(ring_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(ring_test PRIVATE pthread rt)

add_test(NAME RingBufferTests COMMAND ring_test)

# ── Install targets ─────────────────────────────────────────────────────────
install(TARGETS producer consumer DESTINATION bin)

# ── Print configuration summary ─────────────────────────────────────────────
message(STATUS "")
message(STATUS "AeroLog ${PROJECT_VERSION} — Build Configuration")
message(STATUS "  Build type    : ${CMAKE_BUILD_TYPE}")
message(STATUS "  CPU affinity  : ${ENABLE_CPU_AFFINITY}")
message(STATUS "  C++ standard  : ${CMAKE_CXX_STANDARD}")
message(STATUS "")
