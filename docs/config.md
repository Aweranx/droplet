# 配置模块 config

## 核心概念

配置模块解决一个问题：程序里散落各处的参数（端口、级别、超时……）统一注册、
统一从 YAML 加载、值变化时可感知。

- **ConfigVarBase**：一个yaml kv配置项的抽象基类，统一 `toString()/fromString()/getTypeName()`
  接口，让 Config 可以用一张表管理所有不同类型的变量。
- **ConfigVar<T>**：模板子类，真正持有值。带读写锁保护和值变更回调。
- **ConfigConvert**：yaml node与c++对象的转换，可以用偏特化加入不同对象的转换逻辑，如test里手动实现person的偏特化
- **Config**：管理所有的配置项，静态入口负责查找/创建变量、加载 YAML、遍历。

## pipeline

### 注册与读取

使用`LoadFromConfDir`从目录中读取所有的配置文件

程序启动时（或在需要的地方首次使用时）声明配置项，拿到 `ConfigVar<T>` 后
通过 `getValue()/setValue()` 读写：

```cpp
auto port = droplet::Config::Lookup("system.port", 8080, "监听端口");
port->getValue();          // 8080
port->setValue(9090);      // 已注册该变量的回调会被依次通知
port->fromString("1234");  // YAML 字符串 -> int
port->toString();          // int -> YAML 字符串 "1234"
```

### YAML 加载

YAML 树先被 `CollectMember` 深度优先扁平化成 `a.b.c` 形式的键，再逐个与
已注册变量匹配；只有已注册的键会被加载，未注册的键直接忽略：

```yaml
system:
  port: 9092
  name: droplet
```

扁平化后得到 `system.port`、`system.name`，与注册名一致，于是变量被更新。

```
YAML::Node(root)
    │ CollectMember 深度优先扁平化（键逐段转小写）
    ▼
std::list<(key, node)>            "system.port" -> 9092
    │ LoadFromYaml 逐键处理
    ▼
Config::LookupBase(key)           全局表 s_datas 查找（读锁）
    │ 未注册的键 -> 忽略
    ▼
ConfigVarBase::fromString(scalar 或节点整体序列化的 YAML 串)
    │ LexicalCast<std::string, T> 解析；失败记日志并保持原值
    ▼
ConfigVar::setValue(new_value)
    │ 值未变化 -> 直接返回
    ├─ 写锁下完成赋值
    └─ 锁外快照回调表，依次执行 on_change(old, new)
```

## 实现细节

### ConfigConvert
实现`FromNode`和`ToNode`，来完成`YAML::Node`与c++对象的转化。
使用`is_same_v`依次比较是 bool、string、enum还是用户自定义类型，然后调用对应的解析逻辑。
自定义类型需要自己来特化droplet::ConfigConvert<T> 或 YAML::convert<T>。
将`SequenceConvert`和`StringMapConvert`添加为`ConfigConvert`的偏特化分别处理序列容器和map。


### TypeName 编译期取类型名

用 GCC 的 `__PRETTY_FUNCTION__`
在 `constexpr` 中截取 `[with T = ...]` 段，编译期得到
`"int"`、`"std::vector<int>"` 这类可读名称，零运行时开销、零依赖。
类型不匹配的诊断信息（Lookup 返回 nullptr 时）就来自它。

### 读写锁与回调时机

- 每个 `ConfigVar` 一把 `std::shared_mutex`：
  `getValue()/toString()` 走读锁，`setValue()`和回调表增删走写锁。
- `setValue()` 在写锁内完成"比较 + 赋值"，随后在**锁外**快照回调表并逐个
  通知。回调里可以安全调用 `getValue()/toString()`，不会因锁重入死锁
- `Config::Visit()` 同样只持锁做快照，回调在锁外执行。
- `Config` 的全局表与锁是函数局部 static（Meyers 手法），规避静态初始化顺序问题。
- 基本上所有的需要持有配置项的操作都使用先加锁获取配置项snapshot然后再操作，减少阻塞。

### 值变更回调

```cpp
uint64_t id = var->addListener([](const int& old_v, const int& new_v) {
  std::println("port: {} -> {}", old_v, new_v);
});
var->delListener(id);   // 值未变化的 setValue 不会触发回调
```

回调 id 是实例内的自增计数（sylar 用函数内 static，所有同类实例共享一个
计数器，这里改为成员变量，语义更干净）。

### 从目录加载

`Config::LoadFromConfDir(path, force)` 用 `std::filesystem` 枚举目录下全部
`.yml/.yaml`（按路径排序保证顺序确定），记录每个文件的 `last_write_time`：
`force=false` 时 mtime 未变化的文件直接跳过，`force=true` 强制重载。


### 从 YAML 字符串到 bool

`sylar` 依赖 `boost::lexical_cast<bool>`，只认 `"1"/"0"`。这里特化了
`LexicalCast<std::string, bool>`，额外支持 YAML 风格的
`true/false/yes/no/on/off`（大小写不敏感），其余输入抛异常。

## 使用示例

```cpp
#include <droplet/config/config.h>

int main() {
  // 1. 注册配置项（同名重复注册会返回同一实例）
  auto port = droplet::Config::Lookup("system.port", 8080, "监听端口");
  port->addListener([](const int& old_v, const int& new_v) { /* 热更新 */ });

  // 2. 从 YAML 字符串或文件加载
  auto root = YAML::LoadFile("conf.yml");
  droplet::Config::LoadFromYaml(root);
  droplet::Config::LoadFromConfDir("./conf", true);

  // 3. 使用值
  std::println("port = {}", port->getValue());

  // 4. 遍历诊断
  droplet::Config::Visit([](const droplet::ConfigVarBase::Ptr& v) {
    std::println("{} = {}", v->getName(), v->toString());
  });
}
```

注意：配置名称只允许 `[0-9a-z_.]`；`Lookup` 时传入非法名称会抛
`std::invalid_argument`，YAML 中键名非法的节点会被忽略并记录日志。

## 总结

配置模块以"YAML 字符串作为统一序列化载体"为轴心：`LexicalCast` 特化族负责
字符串与各类型（含嵌套容器）的互转，`ConfigVar<T>` 用读写锁保护值并在变化时
通知回调，`Config` 用一张全局表把注册、加载、遍历串起来。

