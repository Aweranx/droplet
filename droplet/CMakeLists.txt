# 自动收集主库的实现和头文件；CONFIGURE_DEPENDS 使文件列表变化时触发重新配置。
# .S 是协程模块 fcontext 的上下文切换汇编（仅限 x86_64 SysV ELF 变体）。
file(GLOB_RECURSE DROPLET_SOURCES CONFIGURE_DEPENDS *.cpp *.cc *.c *.S)
file(GLOB_RECURSE DROPLET_HEADERS CONFIGURE_DEPENDS *.h *.hpp)

# 将全部源码构建为静态库，并让头文件在 IDE 的目标视图中可见。
add_library(droplet STATIC ${DROPLET_SOURCES} ${DROPLET_HEADERS})
add_library(droplet::droplet ALIAS droplet)

redefine_file_macro("${DROPLET_SOURCES}")

# 公共头文件使用 C++23；让下游目标通过 droplet::droplet 自动继承该要求。
target_compile_features(droplet PUBLIC cxx_std_23)

# 构建阶段对使用者公开 include/，安装后则公开标准安装头文件目录。
# src/ 仅供库本身及显式添加该路径的内部测试使用，不向普通使用者传播。
target_include_directories(droplet
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# PUBLIC 使 droplet 本身及其使用者都继承公共编译选项和线程依赖。
target_link_libraries(droplet PUBLIC droplet_options Threads::Threads
    yaml-cpp::yaml-cpp ${CMAKE_DL_LIBS})

if(WIN32)
    target_link_libraries(droplet PRIVATE dbghelp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(droplet PRIVATE PkgConfig::DROPLET_LIBDW)
endif()

target_link_libraries(droplet PUBLIC yaml-cpp::yaml-cpp)

# 第三方依赖的安装目标必须先完成，避免并行构建时主库提前开始编译/链接。
add_dependencies(droplet droplet_third_party_dependencies)


# 主库在通用警告选项之上启用 -Werror，防止带警告的实现代码进入构建产物。
target_compile_options(droplet PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:
        -Werror
    >
)

# 安装库文件并记录到 dropletTargets 导出集合，便于后续生成 CMake 包目标。
install(TARGETS droplet
    EXPORT dropletTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(TARGETS droplet_options EXPORT dropletTargets)

# 保留 include/ 下的相对目录结构安装公开头文件，确保各模块的头文件路径
# 与源码中的 include 路径一致。
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
