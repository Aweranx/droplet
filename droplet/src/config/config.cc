#include "droplet/config/config.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <mutex>
#include <system_error>

namespace droplet {

namespace detail {

bool IsValidConfigName(std::string_view name) noexcept {
  if (name.empty()) {
    return false;
  }
  for (char c : name) {
    const bool valid = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                       c == '.' || c == '_';
    if (!valid) {
      return false;
    }
  }
  return true;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// ConfigVarBase
// ---------------------------------------------------------------------------

ConfigVarBase::ConfigVarBase(std::string name, std::string description)
    : name_(std::move(name)), description_(std::move(description)) {
  // 名称统一转小写，使配置键大小写不敏感（YAML 加载路径依赖这一约定）。
  std::transform(name_.begin(), name_.end(), name_.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
}

// ---------------------------------------------------------------------------
// Config 的非模板成员
// ---------------------------------------------------------------------------

ConfigVarBase::Ptr Config::LookupBase(const std::string& name) {
  ReadLock lock(GetMutex());
  auto it = GetDatas().find(name);
  return it == GetDatas().end() ? nullptr : it->second;
}

/**
 * @brief 深度优先把 YAML 树扁平化成 "a.b.c" 前缀键列表。
 * @details "A.B: 10" 与
 *          A:
 *            B: 10
 *          会被展平成同一个键 "a.b"，标量与节点本体一起进入 output。
 *          每一层键段先转小写再校验，使 YAML 配置键大小写不敏感。
 */
static std::string ToLowerKey(const std::string& key) {
  std::string lower;
  lower.reserve(key.size());
  std::transform(key.begin(), key.end(), std::back_inserter(lower),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return lower;
}

static void ListAllMember(const std::string& prefix, const YAML::Node& node,
                          std::list<std::pair<std::string, YAML::Node>>& output) {
  // 空前缀表示 YAML 根节点本身，直接放行；实际配置键要求合法且非空。
  if (!prefix.empty() && !detail::IsValidConfigName(prefix)) {
    DROPLET_LOG_ERROR(GetRootLogger()) << "配置项名称非法 name=" << prefix;
    return;
  }
  output.emplace_back(prefix, node);
  if (node.IsMap()) {
    for (auto it = node.begin(); it != node.end(); ++it) {
      ListAllMember(prefix.empty() ? ToLowerKey(it->first.Scalar())
                                   : prefix + "." + ToLowerKey(it->first.Scalar()),
                    it->second, output);
    }
  }
}

void Config::LoadFromYaml(const YAML::Node& root) {
  std::list<std::pair<std::string, YAML::Node>> all_nodes;
  ListAllMember("", root, all_nodes);

  for (auto& [key, node] : all_nodes) {
    // 根节点对应的空键没有配置含义，跳过。
    if (key.empty()) {
      continue;
    }

    // 只更新已注册的变量；未注册的键直接忽略。
    ConfigVarBase::Ptr var = LookupBase(key);
    if (!var) {
      continue;
    }

    if (node.IsScalar()) {
      var->fromString(node.Scalar());
    } else {
      // 序列/映射节点整体序列化成 YAML 字符串，交由容器类型的转换逻辑解析。
      std::stringstream ss;
      ss << node;
      var->fromString(ss.str());
    }
  }
}

namespace {

/// 已加载文件的修改时间表，用于 LoadFromConfDir 的增量跳过。
std::map<std::string, std::filesystem::file_time_type>& FileMtimes() {
  static std::map<std::string, std::filesystem::file_time_type> mtimes;
  return mtimes;
}

/// 保护 FileMtimes() 的互斥量（目录加载可能来自不同线程）。
std::mutex g_dir_mutex;

}  // namespace

void Config::LoadFromConfDir(const std::string& path, bool force) {
  std::error_code ec;
  std::filesystem::directory_iterator it(path, ec);
  if (ec) {
    DROPLET_LOG_WARN(GetRootLogger())
        << "LoadFromConfDir 打开目录失败 path=" << path << " what=" << ec.message();
    return;
  }

  // 收集目录下的 .yml/.yaml 文件并按路径排序，保证加载顺序确定。
  std::vector<std::filesystem::path> files;
  for (const auto& entry : it) {
    std::error_code entry_ec;
    if (!entry.is_regular_file(entry_ec) || entry_ec) {
      continue;
    }
    const auto ext = entry.path().extension().string();
    if (ext == ".yml" || ext == ".yaml") {
      files.push_back(entry.path());
    }
  }
  std::sort(files.begin(), files.end());

  for (const auto& file : files) {
    std::error_code time_ec;
    const auto mtime = std::filesystem::last_write_time(file, time_ec);
    if (time_ec) {
      continue;
    }

    {
      std::lock_guard<std::mutex> lock(g_dir_mutex);
      auto& recorded = FileMtimes()[file.string()];
      if (!force && recorded == mtime) {
        continue;  // 文件未变化，跳过重复加载
      }
      recorded = mtime;
    }

    try {
      YAML::Node root = YAML::LoadFile(file.string());
      LoadFromYaml(root);
      DROPLET_LOG_INFO(GetRootLogger())
          << "LoadFromConfDir 加载配置文件成功 file=" << file.string();
    } catch (const std::exception& e) {
      DROPLET_LOG_ERROR(GetRootLogger())
          << "LoadFromConfDir 加载配置文件失败 file=" << file.string()
          << " what=" << e.what();
    }
  }
}

void Config::Visit(const std::function<void(ConfigVarBase::Ptr)>& cb) {
  // 在读锁下只做快照，回调在锁外执行，避免回调内部再进 Config 接口造成死锁。
  std::vector<ConfigVarBase::Ptr> snapshot;
  {
    ReadLock lock(GetMutex());
    auto& datas = GetDatas();
    snapshot.reserve(datas.size());
    for (auto& [name, var] : datas) {
      snapshot.push_back(var);
    }
  }
  for (auto& var : snapshot) {
    cb(var);
  }
}

}  // namespace droplet
