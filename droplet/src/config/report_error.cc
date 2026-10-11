#include <droplet/config/convert.h>
#include <droplet/logger/log.h>

#include <stdexcept>
#include <string>
#include <string_view>

namespace droplet::detail {

namespace {

LoggerPtr ConfigLogger() {
  static LoggerPtr logger = GetLogger("system");
  return logger;
}

}  // namespace

void ReportConfigError(std::string_view operation, std::string_view name,
                       std::string_view type_name, std::string_view reason) {
  auto logger = ConfigLogger();
  DROPLET_LOG_ERROR(logger)
      << operation << " failed:" << " name="
      << (name.empty() ? std::string_view("<none>") : name) << " type="
      << (type_name.empty() ? std::string_view("<none>") : type_name)
      << " reason=" << reason;
}

void ThrowConvertError(std::string_view type_name, std::string_view reason,
                       std::string_view value) {
  std::string message;
  message.reserve(type_name.size() + reason.size() + value.size() + 32);
  message.append("ConfigConvert<")
      .append(type_name)
      .append(">: ")
      .append(reason);
  if (!value.empty()) {
    message.append(", value=").append(value);
  }
  throw std::runtime_error(message);
}

}  // namespace droplet::detail
