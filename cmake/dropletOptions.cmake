# INTERFACE 目标不生成实体文件，仅集中承载并向依赖目标传播公共构建选项。
add_library(droplet_options INTERFACE)

# 为 GCC/Clang 启用通用警告，并关闭严格别名优化以规避相关未定义行为风险。
target_compile_options(droplet_options INTERFACE
    $<$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>:
        -Wall
        -Wextra
        -Wpedantic
        -fno-strict-aliasing
        # 将 __FILE__ 中的项目绝对路径映射为相对于项目根目录的路径。
        -fmacro-prefix-map=${PROJECT_SOURCE_DIR}=.
    >
)

# Linux 上为 GCC/Clang 生成位置无关代码，使静态库也可安全链接进共享库。
target_compile_options(droplet_options INTERFACE
    $<$<AND:$<PLATFORM_ID:Linux>,$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>>:
        -fPIC
    >
)

# -rdynamic 将可执行文件符号加入动态符号表，便于运行时栈回溯解析符号名。
target_link_options(droplet_options INTERFACE
    $<$<AND:$<PLATFORM_ID:Linux>,$<OR:$<CXX_COMPILER_ID:GNU>,$<CXX_COMPILER_ID:Clang>>>:
        -rdynamic
    >
)

# 仅在 Debug 配置中定义项目调试宏，供源码条件编译使用。
target_compile_definitions(droplet_options INTERFACE
    $<$<CONFIG:Debug>:DROPLET_DEBUG>
)

# Debug 关闭优化并生成完整的 GDB 调试信息，便于逐行调试和变量检查。
target_compile_options(droplet_options INTERFACE
    $<$<CONFIG:Debug>:-O0>
    $<$<CONFIG:Debug>:-g3>
    $<$<CONFIG:Debug>:-ggdb>
)

# Release 和 RelWithDebInfo 均启用优化、关闭断言并保留栈帧；后者额外生成调试信息。
target_compile_options(droplet_options INTERFACE
    $<$<CONFIG:Release>:-DNDEBUG>
    $<$<CONFIG:Release>:-O2>
    $<$<CONFIG:Release>:-fno-omit-frame-pointer>
    
    $<$<CONFIG:RelWithDebInfo>:-DNDEBUG>
    $<$<CONFIG:RelWithDebInfo>:-O2>
    $<$<CONFIG:RelWithDebInfo>:-g>
    $<$<CONFIG:RelWithDebInfo>:-fno-omit-frame-pointer>
)

# 可选的覆盖率插桩仅作用于 Debug；编译和链接阶段都必须传入 --coverage。
option(ENABLE_COVERAGE "Enable code coverage instrumentation" OFF)
if(ENABLE_COVERAGE)
    target_compile_options(droplet_options INTERFACE
        $<$<CONFIG:Debug>:--coverage>
    )
    target_link_options(droplet_options INTERFACE
        $<$<CONFIG:Debug>:--coverage>
    )
endif()
