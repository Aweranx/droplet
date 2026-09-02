#pragma once

#include <droplet/export.h>
#include <droplet/logger/log.h>

#include <yaml-cpp/yaml.h>

#include <cctype>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace droplet {

namespace detail {

/**
 * @brief 编译期获取类型 T 的可读名称。
 * @details 借助 GCC 的 __PRETTY_FUNCTION__ 在编译期截取 "[with T = ...]" 段，
 *          替代 sylar 中对 boost::typeindex 的依赖，无运行时开销、无第三方库。
 *          例如 T = int 时返回 "int"，T = std::vector<int> 时返回 "std::vector<int>"。
 */
template <class T>
[[nodiscard]] constexpr std::string_view TypeName() noexcept {
#if defined(__GNUC__)
  std::string_view view = __PRETTY_FUNCTION__;
  // GCC 下的形式：... [with T = int; std::string_view = std::basic_string_view<char>]
  constexpr std::string_view kPrefix = "[with T = ";
  view.remove_prefix(view.find(kPrefix) + kPrefix.size());
  // 先按 ';' 截掉签名尾部的别名说明，再兜底去掉结尾的 ']'。
  const std::size_t end = view.find(';');
  view = view.substr(0, end == std::string_view::npos ? view.size() : end);
  if (!view.empty() && view.back() == ']') {
    view.remove_suffix(1);
  }
  return view;
#else
  return typeid(T).name();
#endif
}

/**
 * @brief 校验配置名称是否合法：仅允许小写字母、数字、'.' 和 '_'。
 * @details LoadFromYaml 会把 YAML 的键统一转成小写后再查找，
 *          因此程序化创建变量时（Config::Lookup）要求名称本身就是小写。
 */
[[nodiscard]] DROPLET_API bool IsValidConfigName(std::string_view name) noexcept;

}  // namespace detail

/**
 * @brief 配置变量的基类。
 * @details 屏蔽具体值类型，使 Config 能够用统一的容器管理所有配置项；
 *          序列化载体统一为 "YAML 格式字符串"。
 */
class DROPLET_API ConfigVarBase {
 public:
  using Ptr = std::shared_ptr<ConfigVarBase>;

  /**
   * @brief 构造函数
   * @param[in] name 配置名称，合法字符为 [0-9a-z_.]，内部会统一转为小写
   * @param[in] description 配置描述
   */
  explicit ConfigVarBase(std::string name, std::string description = "");
  virtual ~ConfigVarBase() = default;

  ConfigVarBase(const ConfigVarBase&) = delete;
  ConfigVarBase& operator=(const ConfigVarBase&) = delete;

  /// 配置名称（小写），如 "logger.level"
  [[nodiscard]] const std::string& getName() const noexcept { return name_; }
  /// 配置描述
  [[nodiscard]] const std::string& getDescription() const noexcept { return description_; }

  /// 将配置值序列化为 YAML 格式字符串；转换异常时记录日志并返回空串。
  [[nodiscard]] virtual std::string toString() const = 0;

  /**
   * @brief 从 YAML 格式字符串反序列化并更新配置值。
   * @return 成功返回 true；失败时记录日志并保持原值不变，返回 false。
   */
  virtual bool fromString(const std::string& val) = 0;

  /// 配置值的类型名称，用于类型不匹配时的诊断信息。
  [[nodiscard]] virtual std::string getTypeName() const = 0;

 protected:
  /// 配置名称，仅允许 [0-9a-z_.]
  std::string name_;
  /// 配置描述
  std::string description_;
};

/**
 * @brief 通用类型转换仿函数（F 源类型，T 目标类型）。
 * @details 默认实现基于 std::stringstream：要求 F 支持 operator<<、T 支持 operator>>。
 *          转换失败（解析不出结果，或解析后残留多余字符）时抛出 std::runtime_error。
 */
template <class F, class T>
struct LexicalCast {
  T operator()(const F& v) const {
    std::stringstream ss;
    ss << v;

    T result{};
    ss >> result;
    if (ss.fail()) {
      throw std::runtime_error("lexical cast failed");
    }
    // 检查残留内容，避免 "42abc" 这类输入被静默截断成 42。
    std::string rest;
    ss >> rest;
    if (!rest.empty()) {
      throw std::runtime_error("lexical cast has trailing characters");
    }
    return result;
  }
};

/// string -> string 直接原样返回；走流转换会被空白截断，破坏 "hello world" 这类值。
template <>
struct LexicalCast<std::string, std::string> {
  std::string operator()(const std::string& v) const { return v; }
};

/// string -> bool，在 "1"/"0" 之外额外支持 YAML 风格的 true/false/yes/no/on/off。
template <>
struct LexicalCast<std::string, bool> {
  bool operator()(const std::string& v) const {
    std::string low;
    low.reserve(v.size());
    for (char c : v) {
      low.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (low == "true" || low == "1" || low == "on" || low == "yes") {
      return true;
    }
    if (low == "false" || low == "0" || low == "off" || low == "no") {
      return false;
    }
    throw std::runtime_error("invalid bool value: " + v);
  }
};

namespace detail {

/// 向容器追加元素：序列容器（vector/list）用 push_back，集合/映射容器用 insert。
template <class Container, class Value>
void InsertTo(Container& c, Value&& v) {
  if constexpr (requires { c.push_back(std::forward<Value>(v)); }) {
    c.push_back(std::forward<Value>(v));
  } else {
    c.insert(std::forward<Value>(v));
  }
}

/**
 * @brief 把 YAML 序列字符串逐元素递归转换成容器。
 * @details 每个元素先还原成 YAML 文本，再递归调用 LexicalCast<string, T>，
 *          因此嵌套容器（如 vector<vector<int>>）天然获得支持。
 */
template <class Container>
Container YamlToSequence(const std::string& val) {
  YAML::Node node = YAML::Load(val);
  Container result;
  using T = typename Container::value_type;
  for (std::size_t i = 0; i < node.size(); ++i) {
    std::stringstream ss;
    ss << node[i];
    detail::InsertTo(result, LexicalCast<std::string, T>()(ss.str()));
  }
  return result;
}

/// 把容器逐元素递归转换成 YAML 序列字符串，是 YamlToSequence 的逆过程。
template <class Container>
std::string SequenceToYaml(const Container& c) {
  YAML::Node node(YAML::NodeType::Sequence);
  using T = typename Container::value_type;
  for (const auto& elem : c) {
    node.push_back(YAML::Load(LexicalCast<T, std::string>()(elem)));
  }
  std::stringstream ss;
  ss << node;
  return ss.str();
}

/// 把 YAML 映射字符串逐值递归转换成映射容器（键保持字符串，值递归转换）。
template <class Container>
Container YamlToMap(const std::string& val) {
  YAML::Node node = YAML::Load(val);
  Container result;
  using T = typename Container::mapped_type;
  for (auto it = node.begin(); it != node.end(); ++it) {
    std::stringstream ss;
    ss << it->second;
    result.emplace(it->first.Scalar(), LexicalCast<std::string, T>()(ss.str()));
  }
  return result;
}

/// 把映射容器逐值递归转换成 YAML 映射字符串，是 YamlToMap 的逆过程。
template <class Container>
std::string MapToYaml(const Container& c) {
  YAML::Node node(YAML::NodeType::Map);
  using T = typename Container::mapped_type;
  for (const auto& [key, value] : c) {
    node[key] = YAML::Load(LexicalCast<T, std::string>()(value));
  }
  std::stringstream ss;
  ss << node;
  return ss.str();
}

}  // namespace detail

// ---------------------------------------------------------------------------
// LexicalCast 的容器片特化：让 YAML 字符串与常见 STL 容器互转。
// 与 sylar 保持一致的支持范围，用户也可按同样的方式为自定义类型添加特化。
// ---------------------------------------------------------------------------

template <class T>
struct LexicalCast<std::string, std::vector<T>> {
  std::vector<T> operator()(const std::string& v) const {
    return detail::YamlToSequence<std::vector<T>>(v);
  }
};

template <class T>
struct LexicalCast<std::vector<T>, std::string> {
  std::string operator()(const std::vector<T>& v) const {
    return detail::SequenceToYaml(v);
  }
};

template <class T>
struct LexicalCast<std::string, std::list<T>> {
  std::list<T> operator()(const std::string& v) const {
    return detail::YamlToSequence<std::list<T>>(v);
  }
};

template <class T>
struct LexicalCast<std::list<T>, std::string> {
  std::string operator()(const std::list<T>& v) const {
    return detail::SequenceToYaml(v);
  }
};

template <class T>
struct LexicalCast<std::string, std::set<T>> {
  std::set<T> operator()(const std::string& v) const {
    return detail::YamlToSequence<std::set<T>>(v);
  }
};

template <class T>
struct LexicalCast<std::set<T>, std::string> {
  std::string operator()(const std::set<T>& v) const {
    return detail::SequenceToYaml(v);
  }
};

template <class T>
struct LexicalCast<std::string, std::unordered_set<T>> {
  std::unordered_set<T> operator()(const std::string& v) const {
    return detail::YamlToSequence<std::unordered_set<T>>(v);
  }
};

template <class T>
struct LexicalCast<std::unordered_set<T>, std::string> {
  std::string operator()(const std::unordered_set<T>& v) const {
    return detail::SequenceToYaml(v);
  }
};

template <class T>
struct LexicalCast<std::string, std::map<std::string, T>> {
  std::map<std::string, T> operator()(const std::string& v) const {
    return detail::YamlToMap<std::map<std::string, T>>(v);
  }
};

template <class T>
struct LexicalCast<std::map<std::string, T>, std::string> {
  std::string operator()(const std::map<std::string, T>& v) const {
    return detail::MapToYaml(v);
  }
};

template <class T>
struct LexicalCast<std::string, std::unordered_map<std::string, T>> {
  std::unordered_map<std::string, T> operator()(const std::string& v) const {
    return detail::YamlToMap<std::unordered_map<std::string, T>>(v);
  }
};

template <class T>
struct LexicalCast<std::unordered_map<std::string, T>, std::string> {
  std::string operator()(const std::unordered_map<std::string, T>& v) const {
    return detail::MapToYaml(v);
  }
};

/**
 * @brief 配置参数模板子类，保存对应类型的参数值。
 * @tparam T 配置值的具体类型，要求可拷贝且支持 == 比较
 * @tparam FromStr 从 YAML 字符串转换成 T 的仿函数
 * @tparam ToStr 从 T 转换成 YAML 字符串的仿函数
 */
template <class T, class FromStr = LexicalCast<std::string, T>,
          class ToStr = LexicalCast<T, std::string>>
class ConfigVar final : public ConfigVarBase {
 public:
  using Ptr = std::shared_ptr<ConfigVar>;
  /// 值变更回调：参数依次为旧值和新值
  using OnChangeCb = std::function<void(const T& old_value, const T& new_value)>;
  using ReadLock = std::shared_lock<std::shared_mutex>;
  using WriteLock = std::unique_lock<std::shared_mutex>;

  /**
   * @brief 通过参数名、默认值、描述构造 ConfigVar
   * @param[in] name 参数名称，合法字符为 [0-9a-z_.]
   * @param[in] default_value 参数默认值
   * @param[in] description 参数描述
   */
  ConfigVar(const std::string& name, const T& default_value,
            const std::string& description = "")
      : ConfigVarBase(name, description), value_(default_value) {}

  [[nodiscard]] std::string toString() const override {
    try {
      ReadLock lock(mutex_);
      return ToStr()(value_);
    } catch (const std::exception& e) {
      DROPLET_LOG_ERROR(GetRootLogger())
          << "ConfigVar::toString 转换失败 name=" << name_
          << " type=" << detail::TypeName<T>() << " what=" << e.what();
    }
    return "";
  }

  bool fromString(const std::string& val) override {
    try {
      setValue(FromStr()(val));
      return true;
    } catch (const std::exception& e) {
      DROPLET_LOG_ERROR(GetRootLogger())
          << "ConfigVar::fromString 转换失败 name=" << name_
          << " type=" << detail::TypeName<T>() << " val=" << val
          << " what=" << e.what();
    }
    return false;
  }

  [[nodiscard]] std::string getTypeName() const override {
    return std::string(detail::TypeName<T>());
  }

  /// 读取当前配置值（读锁下拷贝返回）。
  [[nodiscard]] T getValue() const {
    ReadLock lock(mutex_);
    return value_;
  }

  /**
   * @brief 更新配置值。
   * @details 值未变化时直接返回；发生变化时先在写锁下完成赋值，
   *          再在锁外依次通知所有变更回调，回调中可以安全地调用
   *          getValue()/toString() 而不会产生重入死锁。
   */
  void setValue(const T& v) {
    T old_value;
    {
      WriteLock lock(mutex_);
      if (v == value_) {
        return;
      }
      old_value = value_;
      value_ = v;
    }
    // 锁外快照回调列表，避免持有读锁执行用户代码造成死锁。
    decltype(callbacks_) callbacks;
    {
      ReadLock lock(mutex_);
      callbacks = callbacks_;
    }
    for (auto& [id, cb] : callbacks) {
      cb(old_value, v);
    }
  }

  /**
   * @brief 添加值变更回调。
   * @return 回调对应的唯一 id，用于删除回调
   */
  uint64_t addListener(OnChangeCb cb) {
    WriteLock lock(mutex_);
    callbacks_.emplace(next_listener_id_, std::move(cb));
    return next_listener_id_++;
  }

  /// 删除指定 id 的变更回调。
  void delListener(uint64_t key) {
    WriteLock lock(mutex_);
    callbacks_.erase(key);
  }

  /// 获取指定 id 的回调副本，不存在时返回 nullptr。
  [[nodiscard]] OnChangeCb getListener(uint64_t key) const {
    ReadLock lock(mutex_);
    auto it = callbacks_.find(key);
    return it == callbacks_.end() ? nullptr : it->second;
  }

  /// 清空全部变更回调。
  void clearListeners() {
    WriteLock lock(mutex_);
    callbacks_.clear();
  }

 private:
  mutable std::shared_mutex mutex_;
  /// 配置值
  T value_;
  /// 值变更回调表，key 为实例内自增的唯一 id
  std::map<uint64_t, OnChangeCb> callbacks_;
  /// 下一个回调 id
  uint64_t next_listener_id_{0};
};

/**
 * @brief ConfigVar 的管理类，提供创建 / 查找 / 加载 / 遍历的静态入口。
 * @details 内部用函数局部 static 保存全局配置表与读写锁，
 *          规避静态对象初始化顺序问题（Meyers Singleton 手法）。
 */
class DROPLET_API Config {
 public:
  Config() = delete;

  /**
   * @brief 获取或创建指定名称的配置变量。
   * @details 同名且同类型的变量已存在时直接返回既有实例（保留已加载的值）；
   *          同名但类型不匹配时返回 nullptr 并记录错误日志。
   * @param[in] name 配置名称，仅允许 [0-9a-z_.]
   * @param[in] default_value 不存在时用于创建变量的默认值
   * @param[in] description 配置描述
   * @return 对应的 ConfigVar；名称冲突且类型不匹配时返回 nullptr
   * @exception 名称包含非法字符时抛出 std::invalid_argument
   */
  template <class T>
  static typename ConfigVar<T>::Ptr Lookup(const std::string& name,
                                           const T& default_value,
                                           const std::string& description = "") {
    if (!detail::IsValidConfigName(name)) {
      throw std::invalid_argument("invalid config name: " + name);
    }

    WriteLock lock(GetMutex());
    auto& datas = GetDatas();
    auto it = datas.find(name);
    if (it != datas.end()) {
      if (auto existing = std::dynamic_pointer_cast<ConfigVar<T>>(it->second)) {
        DROPLET_LOG_INFO(GetRootLogger())
            << "Lookup name=" << name << " 已存在，返回既有变量";
        return existing;
      }
      DROPLET_LOG_ERROR(GetRootLogger())
          << "Lookup name=" << name << " 已存在但类型不匹配 type="
          << detail::TypeName<T>() << " real_type=" << it->second->getTypeName()
          << " value=" << it->second->toString();
      return nullptr;
    }

    auto var = std::make_shared<ConfigVar<T>>(name, default_value, description);
    datas.emplace(name, var);
    return var;
  }

  /**
   * @brief 按名称查找已存在的配置变量，不创建。
   * @return 名称不存在或类型不匹配时返回 nullptr
   */
  template <class T>
  static typename ConfigVar<T>::Ptr Lookup(const std::string& name) {
    ReadLock lock(GetMutex());
    auto it = GetDatas().find(name);
    if (it == GetDatas().end()) {
      return nullptr;
    }
    return std::dynamic_pointer_cast<ConfigVar<T>>(it->second);
  }

  /**
   * @brief 使用 YAML::Node 初始化配置模块。
   * @details YAML 树会被扁平化成 "a.b.c" 形式的键，逐项匹配已注册的变量；
   *          没有注册变量的键会被忽略，键名大小写不敏感。
   */
  static void LoadFromYaml(const YAML::Node& root);

  /**
   * @brief 加载目录下全部 .yml/.yaml 配置文件（按文件名顺序）。
   * @param[in] path 配置目录
   * @param[in] force false 时基于文件修改时间增量跳过未变化的文件
   */
  static void LoadFromConfDir(const std::string& path, bool force = false);

  /**
   * @brief 按名称查找配置变量的基类指针，不创建。
   */
  static ConfigVarBase::Ptr LookupBase(const std::string& name);

  /**
   * @brief 遍历当前全部配置项。
   * @param[in] cb 对每个配置项调用的回调；回调在锁外执行，
   *               其中可以安全地调用 Lookup 等接口。
   */
  static void Visit(const std::function<void(ConfigVarBase::Ptr)>& cb);

 private:
  using ConfigVarMap = std::unordered_map<std::string, ConfigVarBase::Ptr>;
  using RWMutexType = std::shared_mutex;
  using ReadLock = std::shared_lock<RWMutexType>;
  using WriteLock = std::unique_lock<RWMutexType>;

  /// 全局配置表
  static ConfigVarMap& GetDatas() {
    static ConfigVarMap datas;
    return datas;
  }

  /// 保护全局配置表的读写锁
  static RWMutexType& GetMutex() {
    static RWMutexType mutex;
    return mutex;
  }
};

}  // namespace droplet
