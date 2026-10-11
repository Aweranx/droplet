#include <droplet/config/config.h>
#include <gtest/gtest.h>

#include <cassert>
#include <filesystem>
#include <list>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

struct Person {
  std::string m_name;
  int m_age{0};
  bool m_sex{false};

  bool operator==(const Person&) const = default;
};
}  // namespace

namespace droplet {

template <>
class ConfigConvert<Person> {
 public:
  [[nodiscard]] static Person FromNode(const YAML::Node& node) {
    return {node["name"].as<std::string>(), node["age"].as<int>(),
            node["sex"].as<bool>()};
  }

  [[nodiscard]] static YAML::Node ToNode(const Person& person) {
    YAML::Node node(YAML::NodeType::Map);
    node["name"] = person.m_name;
    node["age"] = person.m_age;
    node["sex"] = person.m_sex;
    return node;
  }
};
}  // namespace droplet

TEST(TESTCONFIG, config) {
  const std::filesystem::path config_directory =
      std::filesystem::path("../../etc");
  const std::string config_absolute_directory =
      std::filesystem::absolute(config_directory).lexically_normal().string();

  // 在当前程序里直接注册配置项
  auto port = droplet::Config::Lookup("test.port", 8080, "test port");
  auto int_vector = droplet::Config::Lookup(
      "test.int_vec", std::vector<int>{1, 2, 3}, "test int vector");
  auto int_list = droplet::Config::Lookup(
      "test.int_list", std::list<int>{1, 2, 3}, "test int list");
  auto int_set = droplet::Config::Lookup("test.int_set", std::set<int>{1, 2, 3},
                                         "test int set");
  auto int_unordered_set = droplet::Config::Lookup("test.int_unordered_set",
                                                   std::unordered_set<int>{},
                                                   "test int unordered set");
  auto string_int_map = droplet::Config::Lookup(
      "test.str_int_map", std::map<std::string, int>{}, "test string-int map");
  auto string_int_unordered_map = droplet::Config::Lookup(
      "test.str_int_umap", std::unordered_map<std::string, int>{},
      "test string-int umap");
  auto person =
      droplet::Config::Lookup("class.person", Person{}, "test person");
  auto person_map = droplet::Config::Lookup(
      "class.map", std::map<std::string, Person>{}, "test person map");
  auto logs = droplet::Config::Lookup(
      "logs", YAML::Node(YAML::NodeType::Sequence), "logger definiations");

  assert(port && int_vector && int_list && int_set && int_unordered_set &&
         string_int_map && string_int_unordered_map && person && person_map &&
         logs);

  int port_change_count = 0;
  const droplet::ListenerId port_listener_id = port->addListener(
      [&port_change_count](const int& old_value, const int& new_value) {
        assert(old_value == 8080);
        assert(new_value == 5005);
        ++port_change_count;
      });
  EXPECT_NE(port_listener_id, 0);

  int logs_change_count = 0;
  const droplet::ListenerId logs_listener_id = logs->addListener(
      [&logs_change_count](const YAML::Node&, const YAML::Node&) {
        ++logs_change_count;
      });
  EXPECT_NE(logs_listener_id, 0);

  // 读出etc目录下yaml文件里所有的配置项
  droplet::Config::LoadFromConfDir(config_absolute_directory);

  assert(port->getValue() == 5005);
  assert(port_change_count == 1);
  assert(logs_change_count == 1);
  assert(int_vector->getValue() == std::vector<int>({10, 20, 30}));
  assert(int_list->getValue() == std::list<int>({40, 50, 60, 40}));
  assert(int_set->getValue() == std::set<int>({20, 30, 40}));
  assert(int_unordered_set->getValue() ==
         std::unordered_set<int>({20, 30, 40}));
  assert((string_int_map->getValue() ==
          std::map<std::string, int>({{"k", 30}, {"k1", 20}, {"k2", 40}})));
  assert((string_int_unordered_map->getValue() ==
          std::unordered_map<std::string, int>(
              {{"k", 30}, {"k1", 20}, {"k2", 40}})));

  port->removeListener(port_listener_id);
  logs->removeListener(logs_listener_id);
  EXPECT_EQ(port->listenerCount(), 0);
  EXPECT_EQ(logs->listenerCount(), 0);

  port->setValue(6000);
  EXPECT_EQ(port_change_count, 0);
}