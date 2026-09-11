# 自动收集主库的实现和头文件；CONFIGURE_DEPENDS 使文件列表变化时触发重新配置。
# .S 是协程模块 fcontext 的上下文切换汇编（仅限 x86_64 SysV ELF 变体）。
file(GLOB_RECURSE DROPLET_SOURCES CONFIGURE_DEPENDS *.cpp *.cc *.c *.S)
file(GLOB_RECURSE DROPLET_HEADERS CONFIGURE_DEPENDS *.h *.hpp)

# 将全部源码构建为静态库，并让头文件在 IDE 的目标视图中可见。
add_library(droplet STATIC ${DROPLET_SOURCES} ${DROPLET_HEADERS})

# 构建阶段对使用者公开 include/，安装后则公开标准安装头文件目录。
# src/ 仅供库本身及显式添加该路径的内部测试使用，不向普通使用者传播。
target_include_directories(droplet
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# PUBLIC 使 droplet 本身及其使用者都继承 droplet_options 中的编译和链接选项。
# 配置模块的公开头文件直接包含 yaml-cpp 头文件，因此对其公开链接。
target_link_libraries(droplet PUBLIC droplet_options yaml-cpp::yaml-cpp)


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

# 保留 include/ 下的相对目录结构安装公开头文件；否则 iomanager/iomanager.h
# 和顶层转发头 iomanager.h 会被错误地压平到同一个目录并互相覆盖。
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
