#pragma once

#include <droplet/config/config_var.h>
#include <droplet/config/convert.h>
#include <droplet/export.h>
#include <droplet/type_name.h>
#include <yaml-cpp/yaml.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace droplet {

class DROPLET_API Config final {
 public:
  using ConfigVarMap = std::unordered_map<std::string, ConfigVarBase::ptr>;
  using VisitFn = std::function<void(const ConfigVarBase::ptr&)>;

  Config() = delete;

  template <typename T>
  static std::shared_ptr<ConfigVar<T>> Lookup(
      std::string_view name, const T& default_value,
      std::string_view description = {}) {
    ConfigVarBase::ptr base = LookupOrCreate(
        name, TypeName<T>(),
        [&default_value,
         description](std::string normalized_name) -> ConfigVarBase::ptr {
          return std::make_shared<ConfigVar<T>>(std::move(normalized_name),
                                                default_value,
                                                std::string(description));
        });
    if (!base) {
      return nullptr;
    }
    auto typed = std::dynamic_pointer_cast<ConfigVar<T>>(base);
    if (!typed) {
      detail::ReportConfigError("Config::Lookup", base->getName(),
                                TypeName<T>(),
                                "已注册的配置项使用了不同的转换器类型");
    }
    return typed;
  }

  [[nodiscard]] static ConfigVarBase::ptr LookupBase(std::string_view name);

  template <typename T>
  static std::shared_ptr<ConfigVar<T>> Lookup(std::string_view name) {
    return std::dynamic_pointer_cast<ConfigVar<T>>(LookupBase(name));
  }

  static void LoadFromYaml(const YAML::Node& root);
  static void LoadFromConfDir(std::string_view path, bool force = false);
  static void Visit(const VisitFn& callback);

  static void ClearAllListeners();

  [[nodiscard]] static bool IsValidName(std::string_view name) noexcept;

  [[nodiscard]] static std::string NormalizeName(std::string_view name);

  [[nodiscard]] static std::size_t Size();

 private:
  using CreateFn = std::function<ConfigVarBase::ptr(std::string)>;
  static ConfigVarBase::ptr LookupOrCreate(std::string_view name,
                                           std::string_view type_name,
                                           const CreateFn& create);
  static ConfigVarBase::ptr CheckType(ConfigVarBase::ptr var,
                                      std::string_view type_name);
};

}  // namespace droplet