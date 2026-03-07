cmake_minimum_required(VERSION 3.16)
project(jt-zero VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ─── Platform Detection ──────────────────────────────────
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
    set(IS_ARM TRUE)
    message(STATUS "Target: ARM64 (Raspberry Pi)")
    # NEON is default on aarch64, no -mfpu needed
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
    set(IS_ARM TRUE)
    message(STATUS "Target: ARM32 (Raspberry Pi)")
    add_compile_options(-mcpu=cortex-a53 -mfpu=neon-fp-armv8)
else()
    set(IS_ARM FALSE)
    message(STATUS "Target: x86/x64 (Development)")
endif()

# ─── Compiler Flags ──────────────────────────────────────
add_compile_options(
    -Wall -Wextra -Wpedantic
    -O2
    -fno-exceptions      # Embedded: no exceptions
    -fno-rtti            # Embedded: no RTTI
    -ffunction-sections  # Dead code elimination
    -fdata-sections
)

# Link-time optimization
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O3 -DNDEBUG -flto)
    add_link_options(-flto -Wl,--gc-sections)
endif()

# ─── Include Directories ─────────────────────────────────
include_directories(${CMAKE_SOURCE_DIR}/include)

# ─── Source Files ─────────────────────────────────────────
set(CORE_SOURCES
    core/event_engine.cpp
    core/reflex_engine.cpp
    core/rule_engine.cpp
    core/memory_engine.cpp
    core/output_engine.cpp
    core/runtime.cpp
)

set(SENSOR_SOURCES
    sensors/sensors.cpp
)

# ─── Static Library ──────────────────────────────────────
add_library(jtzero_core STATIC
    ${CORE_SOURCES}
    ${SENSOR_SOURCES}
)

target_include_directories(jtzero_core PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(jtzero_core
    pthread
)

# ─── Main Executable ─────────────────────────────────────
add_executable(jt-zero main.cpp)
target_link_libraries(jt-zero jtzero_core pthread)

# ─── Installation ─────────────────────────────────────────
install(TARGETS jt-zero DESTINATION bin)
install(TARGETS jtzero_core DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)

# ─── Cross-Compilation Helper ─────────────────────────────
# Usage: cmake -DCMAKE_TOOLCHAIN_FILE=../toolchain-pi-zero.cmake ..
message(STATUS "")
message(STATUS "╔══════════════════════════════════════╗")
message(STATUS "║        JT-Zero Build System          ║")
message(STATUS "╠══════════════════════════════════════╣")
message(STATUS "║  Version:  ${PROJECT_VERSION}                    ║")
message(STATUS "║  Compiler: ${CMAKE_CXX_COMPILER_ID}               ║")
message(STATUS "║  Standard: C++${CMAKE_CXX_STANDARD}                    ║")
message(STATUS "║  Platform: ${CMAKE_SYSTEM_PROCESSOR}                  ║")
message(STATUS "╚══════════════════════════════════════╝")
message(STATUS "")
