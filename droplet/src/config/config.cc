#include <droplet/config/config.h>
#include <droplet/macros.h>
#include <droplet/utils/fs_utils.h>

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace droplet {

namespace {
using ConfigVarMap = Config::ConfigVarMap;
using FileWriteTimestampMap =
    std::unordered_map<std::string, std::filesystem::file_time_type>;

ConfigVarMap& GetDatas() {
  static ConfigVarMap s_datas;
  return s_datas;
}

std::shared_mutex& GetMutex() {
  static std::shared_mutex s_mutex;
  return s_mutex;
}

FileWriteTimestampMap& GetLoadedFileWriteTimestamps() {
  static FileWriteTimestampMap s_write_timestamps;
  return s_write_timestamps;
}

std::mutex& GetLoadedFileMutex() {
  static std::mutex s_mutex;
  return s_mutex;
}

bool IsFileUnchanged(const std::string& path,
                     const std::filesystem::file_time_type& write_time) {
  std::lock_guard<std::mutex> lock(GetLoadedFileMutex());
  const auto it = GetLoadedFileWriteTimestamps().find(path);
  return it != GetLoadedFileWriteTimestamps().end() && it->second == write_time;
}

void MarkFileLoaded(std::string path,
                    const std::filesystem::file_time_type& write_time) {
  std::lock_guard<std::mutex> lock(GetLoadedFileMutex());
  GetLoadedFileWriteTimestamps().insert_or_assign(std::move(path), write_time);
}

ConfigVarBase::ptr LookupFromMap(const std::string& name) {
  std::shared_lock<std::shared_mutex> lock(GetMutex());
  const auto it = GetDatas().find(name);
  return it == GetDatas().end() ? nullptr : it->second;
}

void CollectMembers(const std::string& prefix, const YAML::Node& node,
                    std::vector<std::pair<std::string, YAML::Node>>& output) {
  if (!prefix.empty()) {
    output.emplace_back(prefix, node);
  }

  if (!node.IsMap()) {
    return;
  }

  for (const auto& entry : node) {
    if (!entry.first.IsScalar()) {
      detail::ReportConfigError("Config::LoadFromYaml", prefix, {},
                                "映射的键必须是标量，已跳过");
      ASSERT_CONTINUE(false);
    }

    std::string key = detail::NormalizeConfigName(entry.first.Scalar());
    if (!detail::IsValidConfigName(key)) {
      detail::ReportConfigError("Config::LoadFromYaml", key, {},
                                "配置名包含非法字符，已跳过");
      ASSERT_CONTINUE(false);
    }
    if (!prefix.empty()) {
      key.insert(0, 1, '.');
      key.insert(0, prefix);
    }
    CollectMembers(key, entry.second, output);
  }
}
}  // namespace

ConfigVarBase::ptr Config::LookupBase(std::string_view name) {
  return LookupFromMap(NormalizeName(name));
}

bool Config::IsValidName(std::string_view name) noexcept {
  return detail::IsValidConfigName(name);
}

std::string Config::NormalizeName(std::string_view name) {
  return detail::NormalizeConfigName(std::string(name));
}

std::size_t Config::Size() {
  std::shared_lock<std::shared_mutex> lock(GetMutex());
  return GetDatas().size();
}

ConfigVarBase::ptr Config::CheckType(ConfigVarBase::ptr var,
                                     std::string_view type_name) {
  if (var->getTypeName() != type_name) {
    detail::ReportConfigError(
        "Config::Lookup", var->getName(), type_name,
        std::string("已注册为其它类型: ").append(var->getTypeName()));
    ASSERT_RETVAL(false, nullptr);
  }
  return var;
}

ConfigVarBase::ptr Config::LookupOrCreate(std::string_view name,
                                          std::string_view type_name,
                                          const CreateFn& create) {
  std::string normalized = NormalizeName(name);
  if (!IsValidName(normalized)) {
    detail::ReportConfigError("Config::Lookup", normalized, type_name,
                              "配置名包含非法字符");
    throw std::invalid_argument("pans: invalid config name: " + normalized);
  }
  if (ConfigVarBase::ptr old_val = LookupFromMap(normalized)) {
    return CheckType(std::move(old_val), type_name);
  }
  ConfigVarBase::ptr new_val = create(normalized);
  if (!new_val) {
    detail::ReportConfigError("Config::Lookup", normalized, type_name,
                              "配置项构造失败");
    return nullptr;
  }

  ConfigVarBase::ptr stored;
  {
    std::unique_lock<std::shared_mutex> lock(GetMutex());
    stored = GetDatas()
                 .emplace(std::move(normalized), std::move(new_val))
                 .first->second;
  }
  return CheckType(std::move(stored), type_name);
}

void Config::LoadFromYaml(const YAML::Node& root) {
  if (!root.IsDefined() || root.IsNull()) {
    return;
  }

  std::vector<std::pair<std::string, YAML::Node>> nodes;
  CollectMembers({}, root, nodes);

  for (const auto& [name, node] : nodes) {
    if (const ConfigVarBase::ptr var = LookupFromMap(name)) {
      var->fromNode(node);
    }
  }
}

void Config::LoadFromConfDir(std::string_view path, bool force) {
  if (path.empty()) {
    detail::ReportConfigError("Config::LoadFromConfDir", {}, {},
                              "配置目录不能为空");
    return;
  }

  std::vector<std::string> files;
  try {
    const std::filesystem::path directory =
        std::filesystem::absolute(std::filesystem::path(path))
            .lexically_normal();
    if (!std::filesystem::is_directory(directory)) {
      detail::ReportConfigError("Config::LoadFromConfDir", directory.string(),
                                {}, "配置目录不存在或不是目录");
      return;
    }
    FSUtils::ListAllFile(files, directory.string(), ".yml");
    FSUtils::ListAllFile(files, directory.string(), ".yaml");
  } catch (const std::exception& e) {
    detail::ReportConfigError("Config::LoadFromConfDir", path, {}, e.what());
    return;
  }
  for (const std::string& file_name : files) {
    try {
      const std::filesystem::file_time_type write_time =
          std::filesystem::last_write_time(file_name);
      if (!force && IsFileUnchanged(file_name, write_time)) {
        continue;
      }
      LoadFromYaml(YAML::LoadFile(file_name));
      MarkFileLoaded(file_name, write_time);
    } catch (const std::exception& e) {
      detail::ReportConfigError("Config::LoadFromConfDir", file_name, {},
                                e.what());
    } catch (...) {
      detail::ReportConfigError("Config::LoadFromConfDir", file_name, {},
                                "未知异常");
    }
  }
}

void Config::Visit(const VisitFn& callback) {
  if (!callback) {
    return;
  }

  std::vector<ConfigVarBase::ptr> snapshot;
  {
    std::shared_lock<std::shared_mutex> lock(GetMutex());
    snapshot.reserve(GetDatas().size());
    for (const auto& [name, var] : GetDatas()) {
      snapshot.push_back(var);
    }
  }
  for (const ConfigVarBase::ptr& var : snapshot) {
    callback(var);
  }
}

void Config::ClearAllListeners() {
  std::vector<ConfigVarBase::ptr> snapshot;
  {
    std::shared_lock<std::shared_mutex> lock(GetMutex());
    snapshot.reserve(GetDatas().size());
    for (const auto& [name, var] : GetDatas()) {
      snapshot.push_back(var);
    }
  }

  for (const ConfigVarBase::ptr& var : snapshot) {
    var->clearListeners();
  }
}
}  // namespace droplet