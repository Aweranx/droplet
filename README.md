## droplet

### os+hardware
win11 + wsl + ubuntu24.04
AMD Ryzen5 9600x 6-core
32g DDR5
Sumsang 990 evo plus 2T

### software
cmake 4.2.2
g++15.2
c++23
GoogleTest 1.18

### build

在项目根目录执行以下命令。第一次配置时会初始化并编译 `third_party/` 中的
git submodule 依赖。

```bash
# 初始化第三方子模块
git submodule update --init --recursive

# Debug 配置（默认构建类型也是 Debug）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug

# 编译主库、示例和测试
cmake --build build --parallel 8
```

构建 Release 版本时，可以使用独立的构建目录：

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel 8
```

已经存在构建目录时，不需要进入该目录，直接从项目根目录重新配置和编译：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 8
```

### test

运行全部 CTest 测试：

```bash
ctest --test-dir build --output-on-failure --parallel 8
```

只编译或运行某个测试目标：

```bash
cmake --build build --target test_logger --parallel 8
./bin/tests/test_logger
```

查看实际编译命令：

```bash
cmake --build build --verbose
```

测试程序输出到 `bin/tests/`，示例程序输出到 `bin/examples/`。
