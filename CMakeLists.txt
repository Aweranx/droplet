# 指定项目要求的最低 CMake 版本。项目使用了较新的生成器表达式和配置能力。
cmake_minimum_required(VERSION 3.31)

# 固定使用系统 GCC/G++，并写入缓存，确保首次及后续配置使用相同编译器。
# FORCE 会覆盖命令行或已有缓存中的编译器设置，如需支持其他工具链可移除此处。
set(CMAKE_C_COMPILER "/usr/bin/gcc" CACHE FILEPATH "C compiler" FORCE)
set(CMAKE_CXX_COMPILER "/usr/bin/g++" CACHE FILEPATH "C++ compiler" FORCE)
# 协程模块的 fcontext 上下文切换是手写汇编（.S），同样固定用 gcc 驱动汇编。
set(CMAKE_ASM_COMPILER "/usr/bin/gcc" CACHE FILEPATH "ASM compiler" FORCE)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

# 声明项目名称、版本及启用的语言。
project(droplet VERSION 0.1.0 LANGUAGES CXX C ASM)

# 给clangd提供解析配置
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

message(STATUS "C compiler version: ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
message(STATUS "C++ compiler version: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")

# GNUInstallDirs 提供标准安装目录变量；CMakePackageConfigHelpers 用于生成包配置文件。
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# 控制可选子模块是否参与构建。示例选项当前仅预留，尚未添加 examples 子目录。
option(DROPLET_BUILD_EXAMPLES "Build examples" ON)
option(DROPLET_BUILD_TESTS    "Build tests"    ON)
option(DROPLET_BUILD_TOOLS    "Build tools"    ON)

# 对 Makefile/Ninja 等单配置生成器，在用户未指定时默认采用 Debug。
# Visual Studio、Xcode 等多配置生成器通过 CMAKE_CONFIGURATION_TYPES 选择配置，不设置此项。
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Choose the type of build." FORCE)
endif()
# 为图形界面及 cmake-gui 声明可选的标准构建类型。
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")

# 根据宿主机逻辑核心数计算默认并行度（核心数的 1.5 倍），供 cmake --build 使用。
cmake_host_system_information(RESULT CPU_NUMS QUERY NUMBER_OF_LOGICAL_CORES)
message(STATUS "CPU NUMS on current server is: ${CPU_NUMS}")
math(EXPR JOBS "${CPU_NUMS} + ${CPU_NUMS} / 2")
set(CMAKE_BUILD_PARALLEL_LEVEL ${JOBS} CACHE STRING "并行编译线程数")
message(STATUS "Compiler default work jobs: ${CMAKE_BUILD_PARALLEL_LEVEL}")

# 定义由所有目标共享的警告、优化、调试和链接选项。
include(cmake/dropletOptions.cmake)

# 配置模块依赖 yaml-cpp 解析 YAML；公开头文件中包含其头文件，故随主库一起编译。
# 与测试侧引入 GoogleTest 的方式保持一致，使用 FetchContent 源码引入，无需系统安装。
include(FetchContent)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "Build yaml-cpp tests" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "Build yaml-cpp tools" FORCE)
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "Build yaml-cpp contrib" FORCE)
set(YAML_CPP_INSTALL OFF CACHE BOOL "Install yaml-cpp" FORCE)
set(YAML_CPP_FORMAT_SOURCE OFF CACHE BOOL "Run clang-format on yaml-cpp" FORCE)
FetchContent_Declare(
    yaml-cpp
    URL https://github.com/jbeder/yaml-cpp/archive/refs/tags/0.8.0.tar.gz
)
# yaml-cpp 0.8.0 声明的最低 CMake 版本低于 3.5，CMake 4.x 已移除该兼容层；
# 通过策略变量放行其旧版声明，仅影响本次子目录引入。
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_MakeAvailable(yaml-cpp)

# 主库始终参与构建；测试和工具按对应开关选择性加入。
add_subdirectory(droplet)
if(DROPLET_BUILD_TESTS)
    # 启用 CTest，并注册 tests 目录中定义的测试用例。
    enable_testing()
    add_subdirectory(tests)
endif()
# if(DROPLET_BUILD_TOOLS)
#     add_subdirectory(tools)
# endif()
