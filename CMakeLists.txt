####################################################################################################
# ProcessMonitor 构建脚本
#
# 用法：
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j
#   ctest --test-dir build --output-on-failure
#
# 可配置选项：
#   -DCOMPILER_TARGET=x86|arm32|arm64     目标架构
#   -DCMAKE_BUILD_TYPE=Release|Debug
#   -DENABLE_SANITIZER=address|thread|undefined|OFF
#   -DWARNINGS_AS_ERRORS=ON|OFF
#   -DTARGET_TYPE=executable|static|shared
####################################################################################################

cmake_minimum_required(VERSION 3.16)

if (NOT DEFINED PROJECT_NAME)
    set(PROJECT_NAME "ProcessMonitor")
endif ()

# 交叉编译工具链必须在 project() 之前设置：旧版把 set(CMAKE_CXX_COMPILER ...)
# 写在 project() 之后，CMake 直接忽略，导致 -DCOMPILER_TARGET=arm64 仍产出 x86 二进制。
set(COMPILER_TARGET "x86" CACHE STRING "Target architecture: x86, arm32, arm64")
if (COMPILER_TARGET STREQUAL "arm64")
    set(CMAKE_SYSTEM_NAME Linux)
    set(CMAKE_SYSTEM_PROCESSOR aarch64)
    set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
elseif (COMPILER_TARGET STREQUAL "arm32")
    set(CMAKE_SYSTEM_NAME Linux)
    set(CMAKE_SYSTEM_PROCESSOR arm)
    # 优先用 PATH 里的工具链；找不到时回退到 Linaro 的常见安装位置，
    # 这样不必把绝对路径硬编码在构建脚本里。
    find_program(PM_ARM32_CXX arm-linux-gnueabihf-g++)
    if (NOT PM_ARM32_CXX)
        find_program(PM_ARM32_CXX arm-linux-gnueabihf-g++
                PATHS /opt/gcc-linaro-7.5.0-2019.12-x86_64_arm-linux-gnueabihf/bin
                NO_DEFAULT_PATH)
    endif ()
    if (NOT PM_ARM32_CXX)
        message(FATAL_ERROR "arm32 toolchain not found: install arm-linux-gnueabihf-g++ "
                "or add it to PATH")
    endif ()
    set(CMAKE_CXX_COMPILER ${PM_ARM32_CXX})
elseif (NOT COMPILER_TARGET STREQUAL "x86")
    message(FATAL_ERROR "Unsupported COMPILER_TARGET: ${COMPILER_TARGET} (x86|arm32|arm64)")
endif ()

project(${PROJECT_NAME} VERSION 1.1.0 LANGUAGES CXX)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type (Release or Debug)" FORCE)
endif ()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ===================== 用户配置变量 =====================
set(TARGET_TYPE "executable" CACHE STRING "Build target type: executable | static | shared")
set(ENABLE_SANITIZER "OFF" CACHE STRING "Sanitizer: OFF | address | thread | undefined")
option(WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(ENABLE_TESTS "Build the regression test suite" ON)
# ========================================================

find_package(Threads REQUIRED)

# ===================== 编译选项 =====================
add_library(pm_warnings INTERFACE)
target_compile_options(pm_warnings INTERFACE
        -Wall -Wextra -Wpedantic
        -Wshadow -Wnon-virtual-dtor
        -Wcast-align -Wunused -Woverloaded-virtual
        -Wnull-dereference -Wdouble-promotion
        -Wformat=2
)
if (WARNINGS_AS_ERRORS)
    target_compile_options(pm_warnings INTERFACE -Werror)
endif ()

if (ENABLE_SANITIZER STREQUAL "address")
    target_compile_options(pm_warnings INTERFACE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(pm_warnings INTERFACE -fsanitize=address)
elseif (ENABLE_SANITIZER STREQUAL "thread")
    target_compile_options(pm_warnings INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(pm_warnings INTERFACE -fsanitize=thread)
elseif (ENABLE_SANITIZER STREQUAL "undefined")
    target_compile_options(pm_warnings INTERFACE -fsanitize=undefined -fno-omit-frame-pointer)
    target_link_options(pm_warnings INTERFACE -fsanitize=undefined)
endif ()

add_library(pm_hardening INTERFACE)
target_compile_options(pm_hardening INTERFACE -fstack-protector-strong)
target_link_options(pm_hardening INTERFACE -Wl,-z,relro -Wl,-z,now)
# ========================================================

# ===================== 核心代码 =====================
set(PM_CORE_SOURCES process.cpp)
set(PM_CORE_HEADERS process.h)

if (TARGET_TYPE STREQUAL "executable")
    add_library(pm_core STATIC ${PM_CORE_SOURCES} ${PM_CORE_HEADERS})
    target_include_directories(pm_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(pm_core PUBLIC pm_warnings PRIVATE Threads::Threads)
    set_target_properties(pm_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
elseif (TARGET_TYPE STREQUAL "static")
    add_library(${PROJECT_NAME} STATIC ${PM_CORE_SOURCES} ${PM_CORE_HEADERS})
    target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${PROJECT_NAME} PUBLIC pm_warnings PRIVATE Threads::Threads)
elseif (TARGET_TYPE STREQUAL "shared")
    add_library(${PROJECT_NAME} SHARED ${PM_CORE_SOURCES} ${PM_CORE_HEADERS})
    target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${PROJECT_NAME} PUBLIC pm_warnings PRIVATE Threads::Threads)
else ()
    message(FATAL_ERROR "Invalid TARGET_TYPE: ${TARGET_TYPE} (executable|static|shared)")
endif ()
# ========================================================

# ===================== 可执行文件 =====================
if (TARGET_TYPE STREQUAL "executable")
    add_executable(${PROJECT_NAME} main.cpp)
    target_link_libraries(${PROJECT_NAME} PRIVATE pm_core pm_hardening)
    set_target_properties(${PROJECT_NAME} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
            POSITION_INDEPENDENT_CODE ON
    )
    install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
    install(FILES config.ini DESTINATION etc/processmonitor)
endif ()
# ========================================================

# ===================== 测试 =====================
if (ENABLE_TESTS AND TARGET_TYPE STREQUAL "executable")
    enable_testing()
    add_subdirectory(tests)
endif ()
# ========================================================
