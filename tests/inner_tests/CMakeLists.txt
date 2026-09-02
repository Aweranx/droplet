# 自动收集内部测试源文件；每个文件会生成一个同名的测试程序。
file(GLOB DROPLET_INNER_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cxx"
)

# 固定目标创建顺序，使配置结果保持一致。
list(SORT DROPLET_INNER_TEST_SOURCES)

# 内部测试与普通测试一样使用平台线程库。
find_package(Threads REQUIRED)

foreach(TEST_SOURCE IN LISTS DROPLET_INNER_TEST_SOURCES)
    # 使用不带扩展名的文件名作为可执行目标名和 CTest 用例名。
    get_filename_component(TEST_NAME "${TEST_SOURCE}" NAME_WE)

    # 检查全局目标名，避免与其他测试目标冲突。
    if(TARGET "${TEST_NAME}")
        message(FATAL_ERROR "Cannot create inner test target '${TEST_NAME}': a target with that name already exists")
    endif()

    add_executable("${TEST_NAME}" "${TEST_SOURCE}")

    # 链接主库，并允许测试直接访问 droplet/src 中的私有实现头文件。
    target_link_libraries("${TEST_NAME}" PRIVATE droplet Threads::Threads)
    target_include_directories("${TEST_NAME}" PRIVATE
        "${PROJECT_SOURCE_DIR}/droplet/src"
    )

    # 与普通测试保持相同的输出位置。
    set_target_properties("${TEST_NAME}" PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${DROPLET_TEST_OUTPUT_DIRECTORY}"
    )

    # 多配置生成器逐项覆盖输出目录，避免追加配置子目录。
    foreach(CONFIG IN LISTS CMAKE_CONFIGURATION_TYPES)
        string(TOUPPER "${CONFIG}" CONFIG_UPPER)
        set_target_properties("${TEST_NAME}" PROPERTIES
            "RUNTIME_OUTPUT_DIRECTORY_${CONFIG_UPPER}"
            "${DROPLET_TEST_OUTPUT_DIRECTORY}"
        )
    endforeach()

    # 注册到 CTest。
    add_test(NAME "${TEST_NAME}" COMMAND "${TEST_NAME}")
endforeach()
