#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/types.h>
#include <unistd.h>

#include <csignal>
#endif

#include <droplet/macros.h>
#include <droplet/utils/fs_utils.h>
#include <droplet/utils/string_utils.h>

namespace droplet {

void FSUtils::ListAllFile(std::vector<std::string>& files,
                          std::string_view path, std::string_view suffix) {
  if (path.empty()) {
    return;
  }

  const std::size_t first_new_file = files.size();
  try {
    const std::filesystem::path root(path);
    if (!std::filesystem::is_directory(root)) {
      return;
    }

    const auto options =
        std::filesystem::directory_options::skip_permission_denied;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::recursive_directory_iterator(root, options)) {
      std::error_code error;
      if (!entry.is_regular_file(error) || error) {
        continue;
      }

      std::string file_name = entry.path().lexically_normal().string();
      if (suffix.empty() || file_name.ends_with(suffix)) {
        files.push_back(std::move(file_name));
      }
    }
  } catch (const std::filesystem::filesystem_error&) {
  }

  std::sort(files.begin() + static_cast<std::ptrdiff_t>(first_new_file),
            files.end());
}

}  // namespace droplet
