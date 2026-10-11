#pragma once

#include <droplet/export.h>

#include <string>
#include <string_view>
#include <vector>

namespace droplet {

class DROPLET_API FSUtils final {
 public:
  FSUtils() = delete;

  static void ListAllFile(std::vector<std::string>& files,
                          std::string_view path, std::string_view suffix = {});
  [[nodiscard]] static bool Mkdir(std::string_view dirname) noexcept;
  [[nodiscard]] static bool IsRunningPidfile(std::string_view pidfile) noexcept;
  [[nodiscard]] static bool Unlink(std::string_view filename,
                                   bool exist = false) noexcept;
};

}  // namespace droplet