#include <droplet/config/config.h>
#include <droplet/logger/log.h>
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <list>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

using droplet::Config;
using droplet::ConfigVar;
using droplet::ConfigVarBase;
using droplet::LexicalCast;

// ---------------------------------------------------------------------------
// LexicalCast 基础转换
// ---------------------------------------------------------------------------

TEST(TestLexicalCast, basic_types) {
  // 模板实参里的逗号会拆散宏参数，直接参与断言的表达式统一加括号。
  EXPECT_EQ((LexicalCast<std::string, int>()("42")), 42);
  EXPECT_EQ((LexicalCast<std::string, int>()("-7")), -7);
  EXPECT_EQ((LexicalCast<int, std::string>()(42)), "42");
  EXPECT_NEAR((LexicalCast<std::string, double>()("3.14")), 3.14, 1e-9);
  // string -> string 走恒等转换，值中的空格不会被截断。
  EXPECT_EQ((LexicalCast<std::string, std::string>()("hello world")),
            "hello world");
}

TEST(TestLexicalCast, invalid_input_throws) {
  EXPECT_THROW((LexicalCast<std::string, int>()("abc")), std::runtime_error);
  // 解析出数值但残留多余字符，同样视为失败。
  EXPECT_THROW((LexicalCast<std::string, int>()("42abc")), std::runtime_error);
}

TEST(TestLexicalCast, bool_style) {
  EXPECT_TRUE((LexicalCast<std::string, bool>()("true")));
  EXPECT_TRUE((LexicalCast<std::string, bool>()("1")));
  EXPECT_TRUE((LexicalCast<std::string, bool>()("on")));
  EXPECT_TRUE((LexicalCast<std::string, bool>()("yes")));
  EXPECT_FALSE((LexicalCast<std::string, bool>()("false")));
  EXPECT_FALSE((LexicalCast<std::string, bool>()("0")));
  EXPECT_FALSE((LexicalCast<std::string, bool>()("off")));
  EXPECT_FALSE((LexicalCast<std::string, bool>()("no")));
  // 大小写不敏感。
  EXPECT_TRUE((LexicalCast<std::string, bool>()("TRUE")));
  EXPECT_THROW((LexicalCast<std::string, bool>()("abc")), std::runtime_error);
}

TEST(TestLexicalCast, sequence_containers) {
  auto vec = LexicalCast<std::string, std::vector<int>>()("[1, 2, 3]");
  EXPECT_EQ(vec, (std::vector<int>{1, 2, 3}));

  auto lst = LexicalCast<std::string, std::list<std::string>>()("[a, b]");
  EXPECT_EQ(lst, (std::list<std::string>{"a", "b"}));

  // set 自动去重并排序。
  auto s = LexicalCast<std::string, std::set<int>>()("[3, 1, 2, 1]");
  EXPECT_EQ(s, (std::set<int>{1, 2, 3}));

  auto us = LexicalCast<std::string, std::unordered_set<int>>()("[1, 1, 2]");
  EXPECT_EQ(us.size(), 2u);
  EXPECT_TRUE(us.count(1));
  EXPECT_TRUE(us.count(2));

  // 空串解析为空容器，便于默认值场景。
  EXPECT_TRUE((LexicalCast<std::string, std::vector<int>>()("").empty()));
}

TEST(TestLexicalCast, map_containers) {
  auto m = LexicalCast<std::string, std::map<std::string, int>>()(
      "a: 1\nb: 2\n");
  EXPECT_EQ(m, (std::map<std::string, int>{{"a", 1}, {"b", 2}}));

  auto um = LexicalCast<std::string, std::unordered_map<std::string, int>>()(
      "x: 10\n");
  EXPECT_EQ(um.at("x"), 10);
}

TEST(TestLexicalCast, round_trip_and_nesting) {
  const std::vector<int> vec{1, 2, 3};
  const auto text = LexicalCast<std::vector<int>, std::string>()(vec);
  // 序列化结果再解析回来应得到同样的内容。
  EXPECT_EQ((LexicalCast<std::string, std::vector<int>>()(text)), vec);

  const std::map<std::string, int> m{{"a", 1}, {"b", 2}};
  const auto m_text = LexicalCast<std::map<std::string, int>, std::string>()(m);
  EXPECT_EQ((LexicalCast<std::string, std::map<std::string, int>>()(m_text)),
            m);

  // 嵌套容器：每个元素递归走一次转换。
  auto nest =
      LexicalCast<std::string, std::vector<std::vector<int>>>()("[[1, 2], [3]]");
  ASSERT_EQ(nest.size(), 2u);
  EXPECT_EQ(nest[0], (std::vector<int>{1, 2}));
  EXPECT_EQ(nest[1], (std::vector<int>{3}));
}

// ---------------------------------------------------------------------------
// ConfigVar 值语义与回调
// ---------------------------------------------------------------------------

TEST(TestConfigVar, value_and_string) {
  auto var = Config::Lookup("test_configvar.value", 8080, "监听端口");
  ASSERT_NE(var, nullptr);
  EXPECT_EQ(var->getName(), "test_configvar.value");
  EXPECT_EQ(var->getDescription(), "监听端口");
  EXPECT_EQ(var->getTypeName(), "int");
  EXPECT_EQ(var->getValue(), 8080);

  var->setValue(9090);
  EXPECT_EQ(var->getValue(), 9090);
  EXPECT_EQ(var->toString(), "9090");

  EXPECT_TRUE(var->fromString("1234"));
  EXPECT_EQ(var->getValue(), 1234);

  // 非法输入：转换失败并保持原值。
  EXPECT_FALSE(var->fromString("not-a-number"));
  EXPECT_EQ(var->getValue(), 1234);
}

TEST(TestConfigVar, listener_notify) {
  auto var = Config::Lookup("test_configvar.listener", std::string("v1"));
  ASSERT_NE(var, nullptr);

  int notify_count = 0;
  std::string old_value;
  std::string new_value;
  const auto id =
      var->addListener([&](const std::string& o, const std::string& n) {
        ++notify_count;
        old_value = o;
        new_value = n;
      });

  var->setValue("v2");
  EXPECT_EQ(notify_count, 1);
  EXPECT_EQ(old_value, "v1");
  EXPECT_EQ(new_value, "v2");

  // 值未变化时不触发回调。
  var->setValue("v2");
  EXPECT_EQ(notify_count, 1);

  // 删除回调后不再触发。
  var->delListener(id);
  EXPECT_EQ(var->getListener(id), nullptr);
  var->setValue("v3");
  EXPECT_EQ(notify_count, 1);
  EXPECT_EQ(var->getValue(), "v3");

  // clearListeners 后重新添加依然可用。
  var->addListener([&](const std::string&, const std::string&) {
    ++notify_count;
  });
  var->setValue("v4");
  EXPECT_EQ(notify_count, 2);
}

// ---------------------------------------------------------------------------
// Config 管理
// ---------------------------------------------------------------------------

TEST(TestConfig, lookup_identity_and_conflict) {
  auto a = Config::Lookup("test_config.lookup", 1);
  auto b = Config::Lookup("test_config.lookup", 100);
  // 同名同类型：返回同一个实例，且不会用新的默认值覆盖已有值。
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(a.get(), b.get());
  EXPECT_EQ(a->getValue(), 1);

  // 同名不同类型：返回 nullptr。
  auto c = Config::Lookup<std::string>("test_config.lookup", "oops");
  EXPECT_EQ(c, nullptr);

  // 名称含非法字符（大写、空格等）时抛出异常。
  EXPECT_THROW(Config::Lookup("test_config.Bad", 1), std::invalid_argument);
  EXPECT_THROW(Config::Lookup("test config", 1), std::invalid_argument);
}

TEST(TestConfig, lookup_existing_only) {
  Config::Lookup("test_config.existing", 7);
  auto found = Config::Lookup<int>("test_config.existing");
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->getValue(), 7);

  // 未注册的名称返回 nullptr，不会创建。
  EXPECT_EQ(Config::Lookup<int>("test_config.missing"), nullptr);

  // LookupBase 能拿到基类指针。
  auto base = Config::LookupBase("test_config.existing");
  ASSERT_NE(base, nullptr);
  EXPECT_EQ(base->getName(), "test_config.existing");
  EXPECT_EQ(base->getTypeName(), "int");
  EXPECT_EQ(Config::LookupBase("test_config.missing"), nullptr);
}

TEST(TestConfig, load_from_yaml) {
  auto port = Config::Lookup("test_yaml.port", 0);
  auto name = Config::Lookup("test_yaml.system.name", std::string());
  auto vec = Config::Lookup<std::vector<std::string>>("test_yaml.vectors", {});
  auto before = Config::Lookup("test_yaml.before_load", 111);
  ASSERT_NE(port, nullptr);
  ASSERT_NE(name, nullptr);
  ASSERT_NE(vec, nullptr);
  ASSERT_NE(before, nullptr);

  const YAML::Node root = YAML::Load(R"(
test_yaml:
  port: 8081
  system:
    name: droplet
  vectors:
    - alpha
    - beta
  unknown_key: not registered
)");
  Config::LoadFromYaml(root);

  // 扁平化：test_yaml.system.name 匹配同名变量。
  EXPECT_EQ(port->getValue(), 8081);
  EXPECT_EQ(name->getValue(), "droplet");
  EXPECT_EQ(vec->getValue(), (std::vector<std::string>{"alpha", "beta"}));

  // 未注册的键被忽略，不影响已有变量。
  EXPECT_EQ(before->getValue(), 111);

  // YAML 键大小写不敏感：大写键转小写后匹配已注册变量。
  const YAML::Node upper = YAML::Load("Test_Yaml:\n  Port: 9092\n");
  Config::LoadFromYaml(upper);
  EXPECT_EQ(port->getValue(), 9092);
}

TEST(TestConfig, load_from_conf_dir) {
  namespace fs = std::filesystem;
  const auto dir = fs::temp_directory_path() / "droplet_test_conf_dir";
  fs::remove_all(dir);
  fs::create_directories(dir);
  const auto file = dir / "a.yml";

  auto var = Config::Lookup("confdir.value", 0);
  ASSERT_NE(var, nullptr);

  auto write_file = [&](const std::string& content) {
    std::ofstream ofs(file, std::ios::binary | std::ios::trunc);
    ofs << content;
  };

  write_file("confdir:\n  value: 100\n");
  Config::LoadFromConfDir(dir.string());
  EXPECT_EQ(var->getValue(), 100);

  // 记录首版 mtime；改写内容后把 mtime 显式拨回原值，
  // force=false 时按增量策略应跳过，不重载。
  const auto first_mtime = fs::last_write_time(file);
  write_file("confdir:\n  value: 200\n");
  fs::last_write_time(file, first_mtime);
  Config::LoadFromConfDir(dir.string());
  EXPECT_EQ(var->getValue(), 100);

  // force=true 强制重载。
  Config::LoadFromConfDir(dir.string(), true);
  EXPECT_EQ(var->getValue(), 200);

  // mtime 确实变化时，force=false 也会重载。
  write_file("confdir:\n  value: 300\n");
  fs::last_write_time(file, first_mtime + std::chrono::seconds(10));
  Config::LoadFromConfDir(dir.string());
  EXPECT_EQ(var->getValue(), 300);

  // 目录不存在时只记录日志，不抛异常。
  EXPECT_NO_THROW(Config::LoadFromConfDir("/nonexistent_dir_for_droplet_test"));

  fs::remove_all(dir);
}

TEST(TestConfig, visit_all) {
  Config::Lookup("test_visit.a", 1);
  Config::Lookup("test_visit.b", std::string("x"));

  std::set<std::string> names;
  Config::Visit(
      [&](const ConfigVarBase::Ptr& var) { names.insert(var->getName()); });
  EXPECT_TRUE(names.count("test_visit.a"));
  EXPECT_TRUE(names.count("test_visit.b"));
}

}  // namespace
