#include <droplet/config/config_var.h>

#include <atomic>
#include <stdexcept>
#include <string>
#include <string_view>

namespace droplet {

namespace detail {

namespace {

constexpr std::string_view CONFIG_NAME_CHARSET =
    "abcdefghijklmnopqrstuvwxyz._0123456789";

}  // namespace

ListenerId NextListenerId() noexcept {
  static std::atomic<ListenerId> s_next{1};
  return s_next.fetch_add(1, std::memory_order_relaxed);
}

std::string NormalizeConfigName(std::string name) {
  for (char& ch : name) {
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  return name;
}

bool IsValidConfigName(std::string_view name) noexcept {
  return !name.empty() &&
         name.find_first_not_of(CONFIG_NAME_CHARSET) == std::string_view::npos;
}

}  // namespace detail

ConfigVarBase::ConfigVarBase(std::string name, std::string description)
    : name_(detail::NormalizeConfigName(std::move(name))),
      description_(std::move(description)) {
  if (!detail::IsValidConfigName(name_)) {
    detail::ReportConfigError("ConfigVarBase::ConfigVarBase", name_, {},
                              "配置名包含非法字符");
    throw std::invalid_argument("pans: invalid config name: " + name_);
  }
}

ConfigVarBase::~ConfigVarBase() = default;

std::string ConfigVarBase::toString() const {
  try {
    const YAML::Node node = toNode();
    if (node.IsScalar()) {
      return node.Scalar();
    }
    YAML::Emitter emitter;
    emitter << node;
    if (!emitter.good()) {
      detail::ReportConfigError("ConfigVarBase::toString", name_,
                                getTypeName(), emitter.GetLastError());
      return {};
    }
    return emitter.c_str();
  } catch (const std::exception& e) {
    detail::ReportConfigError("ConfigVarBase::toString", name_, getTypeName(),
                              e.what());
  } catch (...) {
    detail::ReportConfigError("ConfigVarBase::toString", name_, getTypeName(),
                              "未知异常");
  }
  return {};
}

bool ConfigVarBase::fromString(std::string_view value) {
  try {
    return fromNode(YAML::Load(std::string(value)));
  } catch (const std::exception& e) {
    detail::ReportConfigError("ConfigVarBase::fromString", name_,
                              getTypeName(), e.what());
  } catch (...) {
    detail::ReportConfigError("ConfigVarBase::fromString", name_,
                              getTypeName(), "未知异常");
  }
  return false;
}

}  // namespace droplet