# 每个示例源文件生成一个同名程序，统一输出到 bin/examples。
file(GLOB DROPLET_EXAMPLE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cxx"
)
list(SORT DROPLET_EXAMPLE_SOURCES)

find_package(Threads REQUIRED)

foreach(EXAMPLE_SOURCE IN LISTS DROPLET_EXAMPLE_SOURCES)
    get_filename_component(EXAMPLE_NAME "${EXAMPLE_SOURCE}" NAME_WE)
    add_executable("${EXAMPLE_NAME}" "${EXAMPLE_SOURCE}")
    target_link_libraries("${EXAMPLE_NAME}" PRIVATE droplet Threads::Threads)
    set_target_properties("${EXAMPLE_NAME}" PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/bin/examples"
    )
    foreach(CONFIG IN LISTS CMAKE_CONFIGURATION_TYPES)
        string(TOUPPER "${CONFIG}" CONFIG_UPPER)
        set_target_properties("${EXAMPLE_NAME}" PROPERTIES
            "RUNTIME_OUTPUT_DIRECTORY_${CONFIG_UPPER}"
            "${PROJECT_SOURCE_DIR}/bin/examples"
        )
    endforeach()
endforeach()
