# 收集 tests 根目录下的测试源文件；每个文件会生成一个同名的独立测试程序。
file(GLOB DROPLET_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cxx"
)

# 固定目标创建顺序，使不同文件系统上的配置结果保持一致。
list(SORT DROPLET_TEST_SOURCES)

# 测试程序可能使用线程功能，要求 CMake 提供跨平台的 Threads::Threads 目标。
find_package(Threads REQUIRED)

# GoogleTest 作为测试依赖由 CMake 自动获取，并明确指定 1.18.0 版本。
include(FetchContent)
set(BUILD_GMOCK OFF CACHE BOOL "Build GoogleMock" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "Install GoogleTest" FORCE)
set(gtest_build_tests OFF CACHE BOOL "Build GoogleTest's own tests" FORCE)
if(WIN32)
    set(gtest_force_shared_crt ON CACHE BOOL "Use shared MSVC runtime" FORCE)
endif()

set(DROPLET_GOOGLETEST_VERSION "1.18.0")
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v${DROPLET_GOOGLETEST_VERSION}.tar.gz
)
FetchContent_MakeAvailable(googletest)

# 将所有测试可执行文件集中输出到源码树的 bin/tests 目录。
set(DROPLET_TEST_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/bin/tests")

foreach(TEST_SOURCE IN LISTS DROPLET_TEST_SOURCES)
    # 使用不带扩展名的文件名作为可执行目标名和 CTest 用例名。
    get_filename_component(TEST_NAME "${TEST_SOURCE}" NAME_WE)

    # 提前报告同名目标，避免 CMake 在 add_executable 处给出不易定位的冲突信息。
    if(TARGET "${TEST_NAME}")
        message(FATAL_ERROR "Cannot create test target '${TEST_NAME}': a target with that name already exists")
    endif()

    add_executable("${TEST_NAME}" "${TEST_SOURCE}")

    # droplet 的 PUBLIC 依赖让测试同时继承公开头文件路径和 droplet
    target_link_libraries("${TEST_NAME}" PRIVATE droplet Threads::Threads)

    # 单配置生成器直接使用统一的测试输出目录。
    set_target_properties("${TEST_NAME}" PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${DROPLET_TEST_OUTPUT_DIRECTORY}"
    )

    # 多配置生成器默认会追加 Debug/Release 子目录，这里保持输出路径一致。
    foreach(CONFIG IN LISTS CMAKE_CONFIGURATION_TYPES)
        string(TOUPPER "${CONFIG}" CONFIG_UPPER)
        set_target_properties("${TEST_NAME}" PROPERTIES
            "RUNTIME_OUTPUT_DIRECTORY_${CONFIG_UPPER}" "${DROPLET_TEST_OUTPUT_DIRECTORY}"
        )
    endforeach()

    # 注册到 CTest；测试命令使用对应构建目标生成的可执行文件。
    add_test(NAME "${TEST_NAME}" COMMAND "${TEST_NAME}")
endforeach()

# GoogleTest 用例统一放在 tests/gtest，并通过独立的 CMake 逻辑自动收集。
add_subdirectory(gtest)
