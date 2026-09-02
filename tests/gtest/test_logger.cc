#include <droplet/logger/log.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>

TEST(TestLogger, macro) {
  const std::filesystem::path output_path =
      std::filesystem::temp_directory_path() / "test_logger.log";
  std::ofstream(output_path, std::ios::binary | std::ios::trunc);

  auto logger = std::make_shared<droplet::Logger>("test");
  logger->addAppender(droplet::MakeStdoutAppender());
  logger->addAppender(droplet::MakeFileAppender(output_path.string()));

  DROPLET_LOG_DEBUG(logger) << "debug message";
  DROPLET_LOG_INFO(logger) << "info message";
  DROPLET_LOG_WARN(logger) << "warning message";
  DROPLET_LOG_ERROR(logger) << "error message";
  DROPLET_LOG_FATAL(logger) << "fatal message";
  std::cout << "----------------------------------------------------"
            << std::endl;
  DROPLET_LOG_FMT_DEBUG(logger, "debug message");
  DROPLET_LOG_FMT_INFO(logger, "info message");
  DROPLET_LOG_FMT_WARN(logger, "warn message");
  DROPLET_LOG_FMT_ERROR(logger, "error message");
  DROPLET_LOG_FMT_FATAL(logger, "fatal message");

  char c = 'a';
  int i = 42;
  const char msg1[] = "hello";
  const std::string msg2 = "world";
  const std::string_view msg3 = "droplet 磐石";

  auto g_logger = logger;
  std::cout << "----------------------------------------------------"
            << std::endl;
  LOG_INFO << "c=" << c << " i=" << i << " msg1: " << msg1 << " msg2: " << msg2
           << " msg3: " << msg3;
  DROPLET_LOG_FMT_INFO(logger, "c=%c i=%d msg1: %s msg2: %s msg3: %s", c, i,
                       msg1, msg2.c_str(), msg3.data());

  logger->sync();
  std::cout << "\nThe same records were written to: " << output_path << '\n';
}
