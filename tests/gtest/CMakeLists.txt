# 自动收集 GoogleTest 源文件；新增测试文件后重新配置即可自动加入构建。
file(GLOB DROPLET_GTEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cxx"
)
list(SORT DROPLET_GTEST_SOURCES)

# 让测试中的 __FILE__ 直接使用项目相对路径，不再显示开头的 "./"。
redefine_file_macro("${DROPLET_GTEST_SOURCES}")

include(GoogleTest)

foreach(TEST_SOURCE IN LISTS DROPLET_GTEST_SOURCES)
    get_filename_component(TEST_NAME "${TEST_SOURCE}" NAME_WE)

    if(TARGET "${TEST_NAME}")
        message(FATAL_ERROR "Cannot create test target '${TEST_NAME}': a target with that name already exists")
    endif()

    add_executable("${TEST_NAME}" "${TEST_SOURCE}")
    target_link_libraries("${TEST_NAME}" PRIVATE
        droplet
        GTest::gtest_main
        Threads::Threads
    )

    if(TEST_NAME STREQUAL "test_third_party")
        # 这个测试直接调用几个外部库的 API，确保依赖产物已经安装完成。
        target_link_libraries("${TEST_NAME}" PRIVATE
            droplet::third_party_zlib
            droplet::third_party_tinyxml2
            droplet::third_party_kcp
            droplet::third_party_absl_headers
            droplet::third_party_protobuf_headers
            rapidjson::rapidjson
        )
        add_dependencies("${TEST_NAME}" droplet_third_party_dependencies)
    endif()

    # GoogleTest 示例会访问 logger 的内部头文件；公开 API 测试可以移除此路径。
    target_include_directories("${TEST_NAME}" PRIVATE
        "${PROJECT_SOURCE_DIR}/droplet/src"
    )

    set_target_properties("${TEST_NAME}" PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${DROPLET_TEST_OUTPUT_DIRECTORY}"
    )
    foreach(CONFIG IN LISTS CMAKE_CONFIGURATION_TYPES)
        string(TOUPPER "${CONFIG}" CONFIG_UPPER)
        set_target_properties("${TEST_NAME}" PROPERTIES
            "RUNTIME_OUTPUT_DIRECTORY_${CONFIG_UPPER}"
            "${DROPLET_TEST_OUTPUT_DIRECTORY}"
        )
    endforeach()

    # 从二进制中自动发现 TEST()/TEST_F()/TEST_P()，无需手工逐个 add_test。
    gtest_discover_tests("${TEST_NAME}")
endforeach()
